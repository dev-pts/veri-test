#include "tb-top.h"

#include "verilated.h"
#include "verilated_vcd_c.h"
#include <cstdio>
#include <poll.h>
#include <iostream>
#include <fstream>
#include <map>

struct State;
static bool dumpon;
static std::map<std::string, int> semaphore;
static std::map<std::string, int> cond;
static int done;
static std::vector<State *> st;

static bool is_number(const std::string& s)
{
	std::string::const_iterator it = s.begin();
	while (it != s.end() && std::isdigit(*it)) {
		++it;
	}
	return !s.empty() && it == s.end();
}

struct State {
	FILE *m_in;
	struct pollfd m_pfd;
	std::string m_file;
	unsigned long m_line;
	TOP *m_top;
	enum {
		READ,
		WAIT,
		WAIT_COND,
		VALUE_ASSERT,
		SEMAPHORE,
		DONE,
		FATAL,
		EVAL,
		WAIT_COND_NOTIFY,
	} m_state;
	unsigned long m_cnt;
	std::map<std::string, int> m_wq;
	std::string m_signal;
	std::string m_value;
	std::string m_semaphore;
	std::string m_cond;

	State(std::string file, TOP *top) :
		m_file(file),
		m_line(0),
		m_top(top),
		m_state(READ)
	{
		int fd = std::stoi(file);

		m_in = fdopen(fd, "r");
		setvbuf(m_in, NULL, _IONBF, 0);

		m_pfd.fd = fd;
		m_pfd.events = POLLIN;
	}

	bool set(std::string name, std::string value)
	{
#define SIG(signal, var) else if (name == #signal) { m_top->var = strtoul(value.data(), NULL, 0); return true; }
		if (0) {}
#include "tb-sig.h"
#undef SIG

		fprintf(stderr, "SIGNAL '%s' NOT FOUND IN %s:%li\n", name.data(), m_file.data(), m_line);
		return false;
	}

	bool check(std::string name, int val)
	{
#define SIG(signal, var) else if (name == #signal) { return m_top->var == val; }
		if (0) {}
#include "tb-sig.h"
#undef SIG

		fprintf(stderr, "SIGNAL '%s' NOT FOUND IN %s:%li\n", name.data(), m_file.data(), m_line);
		m_top->contextp()->gotFinish(true);
		m_state = FATAL;
		return false;
	}

	void fatal(const char *msg)
	{
		fprintf(stderr, "FILE ERROR IN %s\n", m_file.data());
		if (msg) {
			fprintf(stderr, "Error: %s\n", msg);
		}
		m_top->contextp()->gotFinish(true);
	}

	void start()
	{
		m_state = READ;
	}

	void step_wait()
	{
		if (m_state != WAIT) {
			return;
		}

		m_cnt--;
		if (m_cnt == 0) {
			m_state = READ;
		}
	}

	void step_cond_notify()
	{
		if (m_state != WAIT_COND_NOTIFY) {
			return;
		}

		auto it = cond.find(m_cond);

		if (it == cond.end()) {
			return;
		}

		m_state = READ;
	}

	void step_cond()
	{
		if (m_state != WAIT_COND) {
			return;
		}

		for (auto it = m_wq.begin(); it != m_wq.end(); it++) {
			if (!check(it->first, it->second)) {
				return;
			}
		}

		m_wq.clear();
		m_state = READ;
	}

	void full_read()
	{
		while (m_state == READ) {
			read();
		}
	}

	std::string next_token(std::string& str)
	{
		auto pos = str.find(' ');
		auto ret = str.substr(0, pos);

		if (pos == std::string::npos) {
			str.erase();
		} else {
			str.erase(0, pos + 1);
		}
		return ret;
	}

	void read()
	{
		if (m_state != READ) {
			return;
		}

		std::string cmd;

		cmd.resize(1024);

		if (poll(&m_pfd, 1, -1) < 0) {
			fatal("poll");
			return;
		}

		if (m_pfd.revents & POLLHUP) {
			fatal("hup");
			return;
		}

		if (!fgets(cmd.data(), cmd.capacity(), m_in)) {
			if (!feof(m_in)) {
				fatal("eof");
				return;
			}
			m_state = DONE;
			return;
		}

		cmd.resize(strlen(cmd.data()) - 1);

		m_line++;

		std::string token = next_token(cmd);

		if (token.size() == 0 || token == "#") {
			return;
		}

		if (is_number(token)) {
			m_cnt = strtoul(token.data(), NULL, 0);
			if (m_cnt > 0) {
				m_state = WAIT;
			}
		} else if (token == "?") {
			std::string name = next_token(cmd);
			int val = strtoul(next_token(cmd).data(), NULL, 0);

			m_wq[name] = val;
		} else if (token == "wait") {
			m_state = WAIT_COND;
		} else if (token == "exit") {
			fprintf(stderr, "EXIT IN %s:%li\n", m_file.data(), m_line);
			m_top->contextp()->gotFinish(true);
		} else if (token == "done") {
			fprintf(stderr, "DONE %s:%li\n", m_file.data(), m_line);
			m_state = DONE;
			fclose(m_in);
			done++;

			if (done == st.size()) {
				m_top->contextp()->gotFinish(true);
			}
		} else if (token == "@") {
			std::string svalue = next_token(cmd);
			std::string name = next_token(cmd);

			int val = strtol(svalue.data(), NULL, 0);

			if (semaphore.find(name) == semaphore.end()) {
				if (val == 0) {
					val = st.size();
				}
				semaphore[name] = val;
			}

			semaphore[name]--;
			m_semaphore = name;
			m_state = SEMAPHORE;
		} else if (token == "-") {
			std::string name = next_token(cmd);

			cond.erase(name);
		} else if (token == "+") {
			std::string name = next_token(cmd);

			cond[name] = 1;
		} else if (token == "=") {
			std::string name = next_token(cmd);

			m_cond = name;
			m_state = WAIT_COND_NOTIFY;
		} else if (token == "dump") {
			dumpon = next_token(cmd) == "on";
		} else {
			m_signal = token;
			m_value = next_token(cmd);
			m_state = EVAL;
		}
	}

	void eval()
	{
		if (m_state != EVAL) {
			return;
		}

		if (!set(m_signal, m_value)) {
			m_top->contextp()->gotFinish(true);
		}

		m_state = READ;
	}
};

int main(int argc, char** argv, char** env)
{
	VerilatedContext* contextp = new VerilatedContext;
	VerilatedVcdC* tfp = new VerilatedVcdC;
	std::string cmd, name, value;

	contextp->commandArgs(argc, argv);
	contextp->traceEverOn(true);

	TOP* top = new TOP { contextp };

	top->trace(tfp, 99);
	tfp->open(argv[1]);

	for (int i = 2; i < argc; i++) {
		st.push_back(new State(argv[i], top));
	}

	while (!contextp->gotFinish()) {
		while (1) {
			bool need_eval = false;
			bool need_read = false;

			for (auto* s : st) {
				s->full_read();
			}

			for (auto* s : st) {
				need_eval |= s->m_state == State::EVAL;
				s->eval();
				need_read |= s->m_state == State::READ;
			}

			/* Evaluate combs */
			if (need_eval) {
				top->eval();
			}

			for (auto* s : st) {
				s->step_cond();
				s->step_cond_notify();
				need_read |= s->m_state == State::READ;
			}

			for (auto it = semaphore.begin(); it != semaphore.end(); ) {
				if (it->second == 0) {
					for (auto* s : st) {
						if (s->m_state == State::SEMAPHORE && it->first == s->m_semaphore) {
							s->start();
						}
					}

					it = semaphore.erase(it);
					need_read = true;
				} else {
					it++;
				}
			}

			if (!need_read) {
				break;
			}
		}

		if (dumpon) {
			tfp->dump(contextp->time());
			contextp->timeInc(1);
		}

#ifdef CLK
		/* Evaluate seqs */
		top->CLK ^= 1;

		top->eval();
#endif

		for (auto* s : st) {
			s->step_wait();
		}
	}

	tfp->dump(contextp->time());
	tfp->close();

	delete top;
	delete contextp;
	return 0;
}
