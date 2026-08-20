#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include "diag.h"

#ifdef DIAG

#define DIAG_DIR	"ur0:data"
#define DIAG_FILE	DIAG_DIR "/udcd_uvc_diag.txt"

static const char diag_hex[16] = "0123456789ABCDEF";

/* Append a raw buffer, opening and closing the file every time so that the
 * log survives a hang, a panic or a failed module load. */
static void diag_emit(const char *buf, int len)
{
	SceUID fd = ksceIoOpen(DIAG_FILE,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 6);
	if (fd < 0)
		return;

	ksceIoWrite(fd, buf, len);
	ksceIoClose(fd);
}

void diag_reset(void)
{
	SceUID fd;

	ksceIoMkdir(DIAG_DIR, 6);

	fd = ksceIoOpen(DIAG_FILE,
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 6);
	if (fd >= 0)
		ksceIoClose(fd);
}

/* Append " 0xXXXXXXXX" to line at *pi. */
static void diag_put_hex(char *line, int *pi, unsigned int v)
{
	int i = *pi;
	int j;

	line[i++] = ' ';
	line[i++] = '0';
	line[i++] = 'x';

	for (j = 28; j >= 0; j -= 4)
		line[i++] = diag_hex[(v >> j) & 0xF];

	*pi = i;
}

/* Append " (dddddddddd)" at *pi, so values can also be read as decimal. */
static void diag_put_dec(char *line, int *pi, unsigned int v)
{
	char tmp[10];
	int n = 0;
	int i = *pi;

	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && n < (int)sizeof(tmp));

	line[i++] = ' ';
	line[i++] = '(';
	while (n--)
		line[i++] = tmp[n];
	line[i++] = ')';

	*pi = i;
}

void diag_stepn(const char *tag, const int *vals, int n)
{
	char line[512];
	int i = 0;
	int k;

	/* Leave room for the values: 24B per value plus the newline. */
	while (tag[i] && i < (int)sizeof(line) - (n * 24) - 4)
		line[i] = tag[i], i++;

	line[i++] = ' ';
	line[i++] = '=';

	for (k = 0; k < n; k++) {
		diag_put_hex(line, &i, (unsigned int)vals[k]);
		diag_put_dec(line, &i, (unsigned int)vals[k]);
	}

	line[i++] = '\n';

	diag_emit(line, i);
}

void diag_step(const char *tag, int code)
{
	char line[128];
	unsigned int v = (unsigned int)code;
	int i = 0;
	int j;

	while (tag[i] && i < (int)sizeof(line) - 14)
		line[i] = tag[i], i++;

	line[i++] = ' ';
	line[i++] = '=';
	line[i++] = ' ';
	line[i++] = '0';
	line[i++] = 'x';

	for (j = 28; j >= 0; j -= 4)
		line[i++] = diag_hex[(v >> j) & 0xF];

	line[i++] = '\n';

	diag_emit(line, i);
}

#endif /* DIAG */
