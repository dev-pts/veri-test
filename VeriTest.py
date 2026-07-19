import os
import subprocess
import sys
import threading

from collections import namedtuple

class Channel:
	def __init__(self, name):
		r, w = os.pipe()

		os.set_blocking(w, True)

		self._name = name
		self._w = w
		self._r = r

	def get_rfd(self):
		return self._r

	def close_wfd(self):
		os.close(self._w)

	def close_rfd(self):
		os.close(self._r)

	def write(self, value):
		os.write(self._w, f'{value}\n'.encode())

class Signal:
	def __init__(self, name, chan):
		self.name = name
		self.chan = chan

	def set(self, value):
		self.chan.write(f'{self.name} {value}')

	def addwait(self, value):
		self.chan.write(f'? {self.name} {value}')

class Router:
	def __init__(self, **kwargs):
		for k, v in kwargs.items():
			if type(v) == dict:
				v = Router(**v)
			elif type(v) == list:
				ret = []
				for i in v:
					ret.append(Router(**i))
				v = ret
			setattr(self, k, v)

		self._inited = True

	def __setattr__(self, key, value):
		if '_inited' in self.__dict__:
			raise Exception('Nothing can be assigned!')
		super().__setattr__(key, value)

class DUT:
	def __init__(self, chan, siglist, task):
		self._chan = chan
		if task:
			self._ev = None
			self._task = task
		else:
			self._ev = EventLoop()
			self._task = lambda dut: dut._ev.loop()
		self.thread = None

		def add(sigs, sig):
			part = sig.split('.')
			for i in range(len(part) - 1):
				name = part[i]

				idx_start = name.find('[')
				idx_end = name.find(']')
				is_array = idx_start >= 0

				if is_array:
					idx = int(name[idx_start + 1:idx_end])
					name = name[:idx_start]

				if name not in sigs:
					if is_array:
						sigs[name] = []
					else:
						sigs[name] = {}

				sigs = sigs[name]

				if is_array:
					while len(sigs) <= idx:
						sigs.append({})
					sigs = sigs[idx]

			sigs[part[-1]] = Signal(sig, chan)

		sigs = {}
		for i in siglist:
			add(sigs, i)

		self.port = Router(**sigs)

	@property
	def ev(self):
		return self._ev

	def wait(self, value=-1):
		if value < 0:
			self._chan.write('wait')
		else:
			self._chan.write(value)

	def semaphore(self, value=0, name=''):
		self._chan.write(f'@ {value} {name}')

	def cond_remove(self, cond):
		self._chan.write(f'- {cond}')

	def cond_wait(self, cond):
		self._chan.write(f'= {cond}')

	def cond_notify(self, cond):
		self._chan.write(f'+ {cond}')

	def finish(self):
		self._chan.write('exit')

	def _done(self):
		self._chan.write('done')

	def dump(self, enable):
		cmd = 'off'
		if enable:
			cmd = 'on'
		self._chan.write(f'dump {cmd}')

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
		self._pid = -1
		self._sema = 0

	def add(self, task=None):
		ret = DUT(Channel(str(task)), self._siglist, task)
		self._tasks.append(ret)
		return ret

	def run(self):
		self.start()
		self.finish()

	def start(self):
		self._pid = os.fork()

		if self._pid == 0:
			# If not closed, then child process will not receive
			# HUP fd state on its side.
			for i in self._tasks:
				i.close_wfd()

			fds = [i.get_rfd() for i in self._tasks]
			subprocess.run([sys.argv[1], self._vcd, *[str(i) for i in fds]], pass_fds=fds)
			sys.exit()

		for i in self._tasks:
			i.close_rfd()

		for i in self._tasks:
			i.thread = threading.Thread(target=i.get_task(), args=(i,))
			i.thread.start()

	def finish(self):
		# First, we wait for the child to end.
		os.waitpid(self._pid, 0)

		# And then, we close our threads.
		for i in self._tasks:
			i.close_wfd()
			i.thread.join()

	def semaphore(self, *args):
		if not args:
			args = self._tasks
		for i in args:
			i.semaphore(len(args), str(self._sema))
		self._sema += 1

	def wait(self, value):
		for i in self._tasks:
			i.wait(value)

class EventLoop:
	def __init__(self):
		self._cv = threading.Condition()
		self._cv_done = threading.Condition()
		self._cmd = []

	def _done(self):
		with self._cv_done:
			self._cmd = []
			self._cv_done.notify()

	def loop(self):
		stop = False

		with self._cv:
			while not stop:
				self._cv.wait_for(lambda: len(self._cmd) > 0)

				for cmd in self._cmd:
					if not cmd:
						stop = True
						break
					cmd()
				else:
					self._done()

		self._done()

	def run(self, *args):
		with self._cv:
			self._cmd.extend(args)
			self._cv.notify()

	def stop(self):
		self.run(None)
		self.wait()

	def wait(self):
		with self._cv_done:
			self._cv_done.wait_for(lambda: len(self._cmd) == 0)

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
