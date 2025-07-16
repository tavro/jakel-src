#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios ot;

void kill(const char *s) {
	perror(s);
	exit(1);
}

void exitRawMode() {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &ot) == -1) kill("tcsetattr");
}

void enterRawMode() {
	if (tcgetattr(STDIN_FILENO, &ot) == -1) kill("tcgetattr");

	atexit(exitRawMode);

	struct termios rt = ot;
	rt.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	rt.c_oflag &= ~(OPOST);
	rt.c_cflag |= (CS8);
	rt.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	rt.c_cc[VMIN] = 0;
	rt.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rt) == -1) kill("tcsetattr");
}

int main() {
	enterRawMode();

	while (1) {
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) kill("read");

		if (iscntrl(c)) {
			printf("%d\r\n", c);
		} else {
			printf("%d ('%c')\r\n", c, c);
		}

		if (c == 'q') break;
	}

	return 0;
}
