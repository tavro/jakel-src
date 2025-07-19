#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define JAKEL_VERSION "0.0.1"
#define JAKEL_TAB_STOP 8
#define JAKEL_QUIT_TIMES 3

#define K_CTRL(k) ((k) & 0x1f)

enum keys {
	BACKSPACE = 127,

	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,

	DELETE,

	HOME,
	END,

	PAGE_UP,
	PAGE_DOWN
};

typedef struct line {
	int size;
	int rsize;

	char *chars;
	char *render;
} line;

struct config {
	int cx, cy;
	int rx;

	int rowOffset;
	int colOffset;

	int rows;
	int cols;

	int rowAmount;
	line *row;

	int modified;

	char *fileName;

	char statusMsg[80];
	time_t statusMsgTime;

	struct termios ot;
};

struct config C;

void setStatusMsg(const char *fmt, ...);
void clear();
char *prompt(char *p, void (*callback)(char*, int));

void kill(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void exitRawMode() {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &C.ot) == -1) kill("tcsetattr");
}

void enterRawMode() {
	if (tcgetattr(STDIN_FILENO, &C.ot) == -1) kill("tcgetattr");

	atexit(exitRawMode);

	struct termios rt = C.ot;

	// Input flags
	// -----------
	// IXON: Disables software flow control (CTRL-S/CTRL-Q).
	//       We turn this off to prevent the terminal from pausing/resuming input.
	// ICRNL: Disables automatic conversion of carriage return ('\r') to newline ('\n').
	//        This ensures CTRL-M and ENTER are treated the same.
	// BRKINT: Disables generation of SIGINT on a break condition.
	//         We don't want the program to be interrupted this way.
	// INPCK: Disables parity checking (not needed for most modern systems).
	// ISTRIP: Disables stripping of the 8th bit of input bytes.
	//         We want to preserve all 8 bits.
	rt.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	// Output flags
	// ------------
	// OPOST: Disables output processing (e.g., converting '\n' to '\r\n').
	//        In raw mode, we want to output data exactly as-is.
	rt.c_oflag &= ~(OPOST);

	// Control flags
	// -------------
	// CS8: Sets character size to 8 bits per byte.
	//      This is a bit mask, not a flag to disable.
	rt.c_cflag |= (CS8);

	// Local flags
	// -----------
	// ECHO: Disables echoing of input characters to the terminal.
	//       Necessary for raw input mode.
	// ICANON: Disables canonical mode, allowing input to be read byte by byte
	//         instead of line by line.
	// ISIG: Disables signal generation for special characters like CTRL-C (SIGINT)
	//       and CTRL-Z (SIGTSTP).
	// IEXTEN: Disables implementation defined input processing (e.g., CTRL-V).
	rt.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	rt.c_cc[VMIN] = 0;
	rt.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rt) == -1) kill("tcsetattr");
}

int readKey() {
	int n;
	char c;

	while ((n = read(STDIN_FILENO, &c, 1)) != 1) {
		if (n == -1 && errno != EAGAIN) kill("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '-') {
					switch (seq[1]) {
						case '1': return HOME;
						case '3': return DELETE;
						case '4': return END;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME;
						case '8': return END;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME;
					case 'F': return END;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME;
				case 'F': return END;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int getCursorPos(int *rs, int *cs) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rs, cs) != 2) return -1;

	return 0;
}

int getSize(int *rs, int *cs) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return getCursorPos(rs, cs);
	} else {
		*cs = ws.ws_col;
		*rs = ws.ws_row;
		return 0;
	}
}

int rowCxToRx(line *row, int cx) {
	int rx = 0;
	int j;
	for ( j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			rx += (JAKEL_TAB_STOP - 1) - (rx % JAKEL_TAB_STOP);
		}
		rx++;
	}

	return rx;
}

int rowRxToCx(line *row, int rx) {
	int curRx = 0;
	int cx;
	for (cx = 0; cx < row->size; cx++) {
		if (row->chars[cx] == '\t') {
			curRx += (JAKEL_TAB_STOP - 1) - (curRx % JAKEL_TAB_STOP);
		}
		curRx++;

		if (curRx > rx) return cx;
	}

	return cx;
}

void updateRow(line *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') tabs++;
	}

	free(row->render);
	row->render = malloc(row->size + tabs*(JAKEL_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % JAKEL_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}

	row->render[idx] = '\0';
	row->rsize = idx;
}

void insertRow(int at, char *s, size_t len) {
	if (at < 0 || at > C.rowAmount) return;

	C.row = realloc(C.row, sizeof(line) * (C.rowAmount + 1));
	memmove(&C.row[at + 1], &C.row[at], sizeof(line) * (C.rowAmount - at));

	C.row[at].size = len;
	C.row[at].chars = malloc(len + 1);

	memcpy(C.row[at].chars, s, len);

	C.row[at].chars[len] = '\0';

	C.row[at].rsize = 0;
	C.row[at].render = NULL;
	updateRow(&C.row[at]);

	C.rowAmount++;
	C.modified++;
}

void freeRow(line* row) {
	free(row->render);
	free(row->chars);
}

void deleteRow(int at) {
	if (at < 0 || at >= C.rowAmount) return;

	freeRow(&C.row[at]);
	memmove(&C.row[at], &C.row[at + 1], sizeof(line) * (C.rowAmount - at - 1));
	C.rowAmount--;
	C.modified++;
}

void insertRowChar(line *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;

	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	updateRow(row);
	C.modified++;
}

void insertChar(int c) {
	if (C.cy == C.rowAmount) {
		insertRow(C.rowAmount, "", 0);
	}

	insertRowChar(&C.row[C.cy], C.cx, c);
	C.cx++;
}

void insertNewLine() {
	if (C.cx == 0) {
		insertRow(C.cy, "", 0);
	} else {
		line *row = &C.row[C.cy];
		insertRow(C.cy + 1, &row->chars[C.cx], row->size - C.cx);
		row = &C.row[C.cy];
		row->size = C.cx;
		row->chars[row->size] = '\0';
		updateRow(row);
	}

	C.cy++;
	C.cx = 0;
}

void appendRowString(line *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	updateRow(row);
	C.modified++;
}

void deleteRowChar(line* row, int at) {
	if (at < 0 || at >= row->size) return;

	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	updateRow(row);
	C.modified++;
}

void deleteChar() {
	if (C.cy == C.rowAmount) return;
	if (C.cx == 0 && C.cy == 0) return;

	line *row = &C.row[C.cy];
	if (C.cx > 0) {
		deleteRowChar(row, C.cx - 1);
		C.cx--;
	} else {
		C.cx = C.row[C.cy - 1].size;
		appendRowString(&C.row[C.cy - 1], row->chars, row->size);
		deleteRow(C.cy);
		C.cy--;
	}
}

char *rowsToString(int *bufLen) {
	int totalLen = 0;

	int j;
	for (j = 0; j < C.rowAmount; j++) totalLen += C.row[j].size + 1;
	*bufLen = totalLen;

	char *buf = malloc(totalLen);
	char *p = buf;
	for (j = 0; j < C.rowAmount; j++) {
		memcpy(p, C.row[j].chars, C.row[j].size);
		p += C.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void openFile(char *fileName) {
	free(C.fileName);
	C.fileName = strdup(fileName);

	FILE *fp = fopen(fileName, "r");
	if (!fp) kill("fopen");

	char *l = NULL;
	size_t lcap = 0;
	ssize_t llen;

	while((llen = getline(&l, &lcap, fp)) != -1) {
		while (llen > 0 && (l[llen - 1] == '\n' || l[llen - 1] == '\r')) llen--;
		insertRow(C.rowAmount, l, llen);
	}

	free(l);
	fclose(fp);
	C.modified = 0;
}

void save() {
	if (C.fileName == NULL) {
		C.fileName = prompt("Save as: %s (ESC to cancel)", NULL);
		if (C.fileName == NULL) {
			setStatusMsg("Save cancel");
			return;
		}
	}

	int len;
	char *buf = rowsToString(&len);

	// O_CREAT tells the program we want to create a new file if it doesn't already exist.
	// O_RDWR tells it we want to open it for reading and writing.
	// 0644 is permissions you would usually want for text files.
	int fd = open(C.fileName, O_RDWR | O_CREAT, 0644);

	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				C.modified = 0;
				setStatusMsg("wrote %d B", len);
				return;
			}
		}
		close(fd);
	}

	free(buf);
	setStatusMsg("I/O error: %s", strerror(errno));
}

void findCallback(char *query, int key) {
	static int lastMatch = -1;
	static int direction = 1;

	if (key == '\r' || key == '\x1b') {
		lastMatch = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		lastMatch = -1;
		direction = 1;
	}

	if (lastMatch == -1) direction = 1;
	int current = lastMatch;

	int i;
	for (i = 0; i < C.rowAmount; i++) {
		current += direction;
		if (current == -1) current = C.rowAmount - 1;
		else if (current == C.rowAmount) current = 0;

		line *row = &C.row[current];
		char *match = strstr(row->render, query);
		if (match) {
			lastMatch = current;
			C.cy = current;
			C.cx = rowRxToCx(row, match - row->render);
			C.rowOffset = C.rowAmount;
			break;
		}
	}
}

void find() {
	int savedCx = C.cx;
	int savedCy = C.cy;
	int savedColOffset = C.colOffset;
	int savedRowOffset = C.rowOffset;

	char *query = prompt("Find: %s (ESC/Arrows/Enter)", findCallback);

	if (query) {
		free(query);
	} else {
		C.cx = savedCx;
		C.cy = savedCy;
		C.colOffset = savedColOffset;
		C.rowOffset = savedRowOffset;
	}
}

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0};

void appendToBuf(struct abuf *ab, const char *s, int len) {
	char *nc = realloc(ab->b, ab->len + len);

	if (nc == NULL) return;
	memcpy(&nc[ab->len], s, len);
	ab->b = nc;
	ab->len += len;
}

void freeBuf(struct abuf *ab) {
	free(ab->b);
}

char *prompt(char *p, void (*callback)(char *, int)) {
	size_t bufSize = 128;
	char *buf = malloc(bufSize);

	size_t bufLen = 0;
	buf[0] = '\0';

	while (1) {
		setStatusMsg(p, buf);
		clear();

		int c = readKey();
		if (c == DELETE || c == K_CTRL('h') || c == BACKSPACE) {
			if (bufLen != 0) buf[--bufLen] = '\0';
		} else if (c == '\x1b') {
			setStatusMsg("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (bufLen != 0) {
				setStatusMsg("");
				if (callback) callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (bufLen == bufSize -1) {
				bufSize *= 2;
				buf = realloc(buf, bufSize);
			}

			buf[bufLen++] = c;
			buf[bufLen] = '\0';
		}

		if (callback) callback(buf, c);
	}
}

void moveCursor(int c) {
	line *row = (C.cy >= C.rowAmount) ? NULL : &C.row[C.cy];

	switch (c) {
		case ARROW_UP:
			if (C.cy != 0) {
				C.cy--;
			}
			break;
		case ARROW_LEFT:
			if (C.cx != 0) {
				C.cx--;
			} else if (C.cy > 0) {
				C.cy--; // IDEA: Stay on same line instead?
				C.cx = C.row[C.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && C.cx < row->size) {
				C.cx++;
			} else if (row && C.cx == row->size) {
				C.cy++; // IDEA: Stay on same line instead?
				C.cx = 0;
			}
			break;
		case ARROW_DOWN:
			if (C.cy < C.rowAmount) {
				C.cy++;
			}
			break;
	}

	row = (C.cy >= C.rowAmount) ? NULL : &C.row[C.cy];
	int rowlen = row ? row->size : 0;
	if (C.cx > rowlen) {
		C.cx = rowlen;
	}
}

void processKey() {
	static int quitTimes = JAKEL_QUIT_TIMES;

	int c = readKey();

	switch (c) {
		case '\r':
			insertNewLine();
			break;

		case K_CTRL('q'):
			if (C.modified && quitTimes > 0) {
				setStatusMsg("UNSAVED FILE: Press CTRL-Q %d times to quit.", quitTimes);
				quitTimes--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
        		write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case K_CTRL('s'):
			save();
			break;

		case HOME:
			C.cx = 0;
			break;
		case END:
			if (C.cy < C.rowAmount) C.cx = C.row[C.cy].size;
			break;

		case K_CTRL('f'):
			find();
			break;

		case BACKSPACE:
		case K_CTRL('h'):
		case DELETE:
			if (c == DELETE) moveCursor(ARROW_RIGHT);
			deleteChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					C.cy = C.rowOffset;
				} else if (c == PAGE_DOWN) {
					C.cy = C.rowOffset + C.rows - 1;
					if (C.cy > C.rowAmount) C.cy = C.rowAmount;
				}

				int t = C.rows;
				while (t--) moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			moveCursor(c);
			break;

		case K_CTRL('l'):
		case '\x1b':
			break;

		default:
			insertChar(c);
			break;
	}

	quitTimes = JAKEL_QUIT_TIMES;
}

void scroll() {
	C.rx = 0;
	if (C.cy < C.rowAmount) {
		C.rx = rowCxToRx(&C.row[C.cy], C.cx);
	}

	if (C.cy < C.rowOffset) {
		C.rowOffset = C.cy;
	}

	if (C.cy >= C.rowOffset + C.rows) {
		C.rowOffset = C.cy - C.rows + 1;
	}

	if (C.rx < C.colOffset) {
		C.colOffset = C.rx;
	}

	if (C.rx >= C.colOffset + C.cols) {
		C.colOffset = C.rx - C.cols + 1;
	}
}

void draw(struct abuf *ab) {
	int row;
	for (row = 0; row < C.rows; row++) {
		int fileRow = row + C.rowOffset;
		if (fileRow >= C.rowAmount) {
			if (C.rowAmount == 0 && row == C.rows / 3) {
				char headerMsg[80];
				int headerMsgLen = snprintf(headerMsg, sizeof(headerMsg), "jakel -- version %s", JAKEL_VERSION);

				if (headerMsgLen > C.cols) headerMsgLen = C.cols;

				int padding = (C.cols - headerMsgLen) / 2;
				if (padding) {
					appendToBuf(ab, "-", 1);
					padding--;
				}
				while (padding--) appendToBuf(ab, " ", 1);

				appendToBuf(ab, headerMsg, headerMsgLen);
			} else {
				appendToBuf(ab, "-", 1);
			}
		} else {
			int len = C.row[fileRow].rsize - C.colOffset;
			if (len < 0) len = 0;
			if (len > C.cols) len = C.cols;
			appendToBuf(ab, &C.row[fileRow].render[C.colOffset], len);
		}

		appendToBuf(ab, "\x1b[K", 3);
		appendToBuf(ab, "\r\n", 2);
	}
}

void drawStatusBar(struct abuf *ab) {
	appendToBuf(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", C.fileName ? C.fileName : "[Unnamed]", C.rowAmount, C.modified ? "(M)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", C.cy + 1, C.rowAmount);

	if (len > C.cols) len = C.cols;
	appendToBuf(ab, status, len);

	while (len < C.cols) {
		if (C.cols - len == rlen) {
			appendToBuf(ab, rstatus, rlen);
			break;
		} else {
			appendToBuf(ab, " ", 1);
			len++;
		}
	}

	appendToBuf(ab, "\x1b[m", 3);
	appendToBuf(ab, "\r\n", 2);
}

void drawMsgBar(struct abuf *ab) {
	appendToBuf(ab, "\x1b[K", 3);

	int msgLen = strlen(C.statusMsg);
	if (msgLen > C.cols) msgLen = C.cols;
	if (msgLen && time(NULL) - C.statusMsgTime < 5) appendToBuf(ab, C.statusMsg, msgLen);
}

void clear() {
	scroll();

	struct abuf ab = ABUF_INIT;

	appendToBuf(&ab, "\x1b[?25l", 6);
	appendToBuf(&ab, "\x1b[H", 3);

	draw(&ab);
	drawStatusBar(&ab);
	drawMsgBar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (C.cy - C.rowOffset) + 1, (C.rx - C.colOffset) + 1);
	appendToBuf(&ab, buf, strlen(buf));

	appendToBuf(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	freeBuf(&ab);
}

void setStatusMsg(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(C.statusMsg, sizeof(C.statusMsg), fmt, ap);
	va_end(ap);
	C.statusMsgTime = time(NULL);
}

void init() {
	C.cx = 0;
	C.cy = 0;
	C.rx = 0;
	C.rowOffset = 0;
	C.colOffset = 0;
	C.rowAmount = 0;
	C.row = NULL;
	C.modified = 0;
	C.fileName = NULL;
	C.statusMsg[0] = '\0';
	C.statusMsgTime = 0;

	if (getSize(&C.rows, &C.cols) == -1) kill("getSize");

	C.rows -= 2;
}

int main(int argc, char *argv[]) {
	enterRawMode();
	init();

	if (argc >= 2) {
		openFile(argv[1]);
	}

	setStatusMsg("CTRL-S(ave) | CTRL-Q(uit) | CTRL-F(ind)");

	while (1) {
		clear();
		processKey();
	}

	return 0;
}

