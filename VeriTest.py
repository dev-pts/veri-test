import subprocess
import sys
import os
import threading
import select
import time

class Channel:
	def __init__(self, name):
		r, w = os.pipe()

		os.set_blocking(w, True)

		self._name = name
		self._w = w
		self._r = r
		self._poll = select.poll()

		self._poll.register(self._r, select.POLLIN)

	def get_rfd(self):
		return self._r

	def close_wfd(self):
		os.close(self._w)

	def close_rfd(self):
		os.close(self._r)

	def write(self, value):
		os.write(self._w, f'{value}\n'.encode())

		# Wait until read happens
		while True:
			events = self._poll.poll(0)
			if not events:
				break
			if events[0][1] & select.POLLNVAL:
				break
			# Schedule to another thread
			time.sleep(0)

class Signal:
	def __init__(self, name, chan):
		self.name = name
		self.chan = chan

	def set(self, value):
		self.chan.write(f'{self.name} {value}')

	def wait(self, value):
		self.chan.write(f'? {self.name} {value}')

	def mustbe(self, value):
		self.chan.write(f'! {self.name} {value}')

class Port:
	def __init__(self, chan, siglist):
		self._sig = {}

		for i in siglist:
			self._sig[i] = Signal(i, chan)

	def __getattr__(self, name):
		if '_sig' in self.__getattribute__('__dict__'):
			return self._sig[name]
		raise Exception(name)

	def __setattr__(self, name, value):
		if '_sig' in self.__getattribute__('__dict__'):
			self._sig[name].set(value)
			return
		object.__setattr__(self, name, value)

class DUT:
	def __init__(self, chan, siglist, task):
		self._chan = chan
		self._task = task
		self.port = Port(chan, siglist)
		self.thread = None

	def wait(self, value):
		self._chan.write(value)

	def semaphore(self):
		self._chan.write('semaphore')

	def finish(self):
		self._chan.write('exit')

	def _done(self):
		self._chan.write('done')

	def dump(self, enable):
		cmd = 'dumpoff'
		if enable:
			cmd = 'dumpon'
		self._chan.write(cmd)

	def get_rfd(self):
		return self._chan.get_rfd()

	def close_wfd(self):
		return self._chan.close_wfd()

	def close_rfd(self):
		return self._chan.close_rfd()

	def _task_wrapper(self):
		self._task(self)
		self._done()

	def get_task(self):
		return DUT._task_wrapper

class VeriTest:
	def __init__(self, siglist, vcd):
		self._siglist = siglist
		self._tasks = []
		self._vcd = vcd

	def add(self, task):
		ret = DUT(Channel(str(task)), self._siglist, task)
		self._tasks.append(ret)
		return ret

	def run(self):
		pid = os.fork()

		if pid == 0:
			# If not closed, then child process will not receive
			# HUP fd state on its side.
			for i in self._tasks:
				i.close_wfd()

			fds = [i.get_rfd() for i in self._tasks]
			subprocess.run([sys.argv[1], self._vcd, *[str(i) for i in fds]], pass_fds=fds)
			sys.exit()

		for i in self._tasks:
			i.thread = threading.Thread(target=i.get_task(), args=(i,))
			i.thread.start()

		# First, we wait for the child to end.
		os.waitpid(pid, 0)

		# And then, we close our threads.
		for i in self._tasks:
			i.close_rfd()
			i.close_wfd()
			i.thread.join()

class UART:
	def __init__(self, dut, sig, div):
		self._dut = dut
		self._sig = sig
		self._div = int(div)

	def _wait(self, v):
		self._dut.wait(self._div * 2 * v)

	def tx(self, data):
		for c in data:
			self._sig.set(0)
			self._wait(1)

			for i in range(8):
				self._sig.set((c >> i) & 1)
				self._wait(1)

			self._sig.set(1)
			self._wait(1)

	def break_cond(self):
		self._sig.set(0)
		self._wait(100)

		self._sig.set(1)
		self._wait(1)
