#include "tb-top.h"

#include "verilated.h"
#include "verilated_vcd_c.h"
#include <cstdio>
#include <poll.h>
#include <iostream>
#include <fstream>

static bool dumpon;
static int semaphore;

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
		WAIT_COND_WIRE,
		VALUE_ASSERT,
		DONE,
		FATAL,
	} m_state;
	unsigned long m_cnt;
	std::string m_name;
	unsigned long m_val;

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
#define SIG(signal, var) else if (name == #signal) { m_top->var = strtoul(value.data(), NULL, 0); }
		if (0) {}
#include "tb-sig.h"
		else {
			fprintf(stderr, "SIGNAL '%s' NOT FOUND IN %s:%li\n", name.data(), m_file.data(), m_line);
			return false;
		}
#undef SIG

		return true;
	}

	bool check()
	{
#define COND(signal, var) else if (m_name == #signal) { return m_top->var == m_val; }
		if (0) {}
#include "tb-cond.h"
		else {
			fprintf(stderr, "SIGNAL '%s' NOT FOUND IN %s:%li\n", m_name.data(), m_file.data(), m_line);
			m_state = FATAL;
			return false;
		}
#undef COND

		return false;
	}

	bool value_assert()
	{
		if (!check()) {
			if (m_state == FATAL) {
				m_top->contextp()->gotFinish(true);
				return false;
			}
			fprintf(stderr, "ASSERT FAILED AT %s:%li\n", m_file.data(), m_line);
			m_state = DONE;
			m_top->contextp()->gotFinish(true);
			return false;
		}

		m_state = READ;
		return true;
	}

	bool cond()
	{
		switch (m_state) {
		case WAIT_COND:
			if (!check()) {
				if (m_state == FATAL) {
					m_top->contextp()->gotFinish(true);
					return false;
				}
				return true;
			}

			m_state = READ;
			return true;
		case VALUE_ASSERT:
			return value_assert();
		}

		return true;
	}

	void fatal()
	{
		fprintf(stderr, "FILE ERROR IN %s\n", m_file.data());
		m_top->contextp()->gotFinish(true);
	}

	void start()
	{
		m_state = READ;
	}

	bool step()
	{
		switch (m_state) {
		case WAIT:
			m_cnt--;
			if (m_cnt == 0) {
				m_state = READ;
				break;
			}
			return true;
		case WAIT_COND:
			return true;
		case VALUE_ASSERT:
			return true;
		case WAIT_COND_WIRE:
			if (!check()) {
				if (m_state == FATAL) {
					m_top->contextp()->gotFinish(true);
					return false;
				}
				return true;
			}

			m_state = READ;
			break;
		case DONE:
			return true;
		case READ:
			break;
		default:
			fprintf(stderr, "FATAL STATE\n");
			m_top->contextp()->gotFinish(true);
			return false;
		}

		std::string cmd;

		do {
			cmd.resize(1024);

			if (poll(&m_pfd, 1, -1) < 0) {
				fatal();
				return false;
			}

			if (m_pfd.revents & POLLHUP) {
				fatal();
				return false;
			}

			if (!fgets(cmd.data(), cmd.capacity(), m_in)) {
				if (!feof(m_in)) {
					fatal();
					return false;
				}
				m_state = DONE;
				return true;
			}

			cmd.resize(strlen(cmd.data()) - 1);

			m_line++;

			if (cmd[0] == '#' || cmd.size() == 0) {
				continue;
			}

			if (is_number(cmd)) {
				m_cnt = strtoul(cmd.data(), NULL, 0);
				if (m_cnt == 0) {
					return true;
				}
				m_state = WAIT;
				return true;
			} else if (cmd[0] == '!') {
				cmd = cmd.substr(cmd.find(' ') + 1);
				m_name = cmd.substr(0, cmd.find(' '));
				cmd.erase(0, cmd.find(' ') + 1);
				m_val = strtoul(cmd.data(), NULL, 0);

				if (!value_assert()) {
					return false;
				}
				break;
			} else if (cmd[0] == '?') {
				if (cmd[1] == '>') {
					m_state = WAIT_COND_WIRE;
				} else {
					m_state = WAIT_COND;
				}

				cmd = cmd.substr(cmd.find(' ') + 1);
				m_name = cmd.substr(0, cmd.find(' '));
				cmd.erase(0, cmd.find(' ') + 1);
				m_val = strtoul(cmd.data(), NULL, 0);

				if (m_state == WAIT_COND_WIRE) {
					if (!check()) {
						if (m_state == FATAL) {
							m_top->contextp()->gotFinish(true);
							return false;
						}
					} else {
						m_state = READ;
						break;
					}
				}
				return true;
			} else if (cmd == "exit") {
				fprintf(stderr, "EXIT IN %s:%li\n", m_file.data(), m_line);
				m_top->contextp()->gotFinish(true);
				return false;
			} else if (cmd == "done") {
				fprintf(stderr, "DONE %s:%li\n", m_file.data(), m_line);
				m_state = DONE;
				fclose(m_in);
				return false;
			} else if (cmd == "semaphore") {
				semaphore++;
				m_state = DONE;
				return false;
			} else if (cmd == "dumpon") {
				dumpon = true;
			} else if (cmd == "dumpoff") {
				dumpon = false;
			} else {
				std::string name = cmd.substr(0, cmd.find(' '));
				cmd.erase(0, cmd.find(' ') + 1);

				if (!set(name, cmd)) {
					m_top->contextp()->gotFinish(true);
					return false;
				}
			}
		} while (false);

		return true;
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

	std::vector<State *> st;
	for (int i = 2; i < argc; i++) {
		st.push_back(new State(argv[i], top));
	}

	while (!contextp->gotFinish()) {
		for (auto* s : st) {
			s->step();
		}

		if (semaphore == st.size()) {
			for (auto* s : st) {
				s->start();
			}
			semaphore = 0;
		}

		/* Evaluate combs */
		top->eval();

		if (dumpon) {
			tfp->dump(contextp->time());
			contextp->timeInc(1);
		}

		/* Evaluate seqs */
		top->clk ^= 1;

		if (top->clk) {
			for (auto* s : st) {
				s->cond();
			}
		}

		top->eval();
	}

	tfp->dump(contextp->time());
	tfp->close();

	delete top;
	delete contextp;
	return 0;
}
