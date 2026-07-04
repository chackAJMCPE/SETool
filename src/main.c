// SETool - A Work In Progress EDL tool for exynos chips (S21 and later)
// Code under GNU GPL v3.0 or later
// Created by chackAJMCPE@github.com

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <time.h>

#ifndef CRTSCTS
#define CRTSCTS 0
#endif

#define BAUD_RATE       B115200
#define READ_TIMEOUT_MS 200
#define BLOCK_SIZE      10240
#define HEADER_SIZE     4096

static const uint8_t OP_DNW[] = { 0x1B, 'D', 'N', 'W' };

typedef struct {
    const char *name;
    char       *path;
    uint8_t    *data;
    size_t      size;
} Image;

#define MAX_IMAGES 32
static bool  debug_raw = false;
static Image images[MAX_IMAGES];
static int   n_images = 0;

static Image *find_image(const char *name)
{
    for (int i = 0; i < n_images; i++)
        if (strcmp(images[i].name, name) == 0)
            return &images[i];
    return NULL;
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static uint8_t *load_image(const char *src_dir, const char *filename, size_t *out_size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", src_dir, filename);
    uint8_t *d = read_file(path, out_size);
    if (d) return d;
    return read_file(filename, out_size);
}

static int open_serial(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("open"); return -1; }
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { perror("tcgetattr"); close(fd); return -1; }
    cfsetospeed(&tty, BAUD_RATE);
    cfsetispeed(&tty, BAUD_RATE);
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_iflag  = IGNBRK;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { perror("tcsetattr"); close(fd); return -1; }
    return fd;
}

// Write all `len` bytes to fd, handling short writes and EAGAIN on a
// non-blocking fd (mirrors how a non-blocking write loop must behave).
static bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            int r = select(fd + 1, NULL, &fds, NULL, &tv);
            if (r <= 0) return false; // timeout or error waiting to write 
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false; // real error
    }
    return true;
}

static int read_line(int fd, char *buf, size_t maxlen)
{
    size_t pos = 0;
    struct timeval tv;
    while (pos < maxlen - 1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = 0;
        tv.tv_usec = READ_TIMEOUT_MS * 1000;
        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r < 0)  return -1;
        if (r == 0) break;
        uint8_t c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0)  return -1;
        if (n == 0) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            if (pos > 0) { buf[pos] = '\0'; return (int)pos; }
            continue;
        }
        buf[pos++] = (char)c;
    }
    if (pos > 0) { buf[pos] = '\0'; return (int)pos; }
    return 0;
}

static uint16_t calc_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

static bool write_dnw(int fd, const uint8_t *data, size_t len)
{
    uint32_t frame_len = (uint32_t)(4 + 4 + len + 2);
    uint16_t csum      = calc_checksum(data, len);

    size_t total = 8 + len + 2;
    uint8_t *buf = malloc(total);
    if (!buf) return false;

    memcpy(buf, OP_DNW, 4);
    buf[4] = (frame_len      ) & 0xFF;
    buf[5] = (frame_len >>  8) & 0xFF;
    buf[6] = (frame_len >> 16) & 0xFF;
    buf[7] = (frame_len >> 24) & 0xFF;
    memcpy(buf + 8, data, len);
    buf[8 + len]     = (uint8_t)(csum & 0xFF);
    buf[8 + len + 1] = (uint8_t)(csum >> 8);
    size_t written = 0;
    while (written < total) {
        size_t chunk = total - written;
        if (chunk > BLOCK_SIZE) chunk = BLOCK_SIZE;
        if (!write_all(fd, buf + written, chunk)) { free(buf); return false; }
        written += chunk;
    }

    free(buf);
    return true;
}
typedef struct {
    char cmd[64];
    char sub[64];
    char dev[128];
    char arg[64];
} Msg;

static void parse_msg(const char *line, Msg *m)
{
    memset(m, 0, sizeof(*m));
    const char *p = line;
    char *dst[] = { m->cmd, m->sub, m->dev, m->arg };
    size_t lims[] = { sizeof(m->cmd), sizeof(m->sub), sizeof(m->dev), sizeof(m->arg) };
    for (int i = 0; i < 4; i++) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n >= lims[i]) n = lims[i] - 1;
        memcpy(dst[i], p, n);
        dst[i][n] = '\0';
        if (!colon) break;
        p = colon + 1;
    }
    size_t clen = strlen(m->cmd);
    if (clen > 12 && strcmp(m->cmd + clen - 12, " header fail") == 0) {
        m->cmd[clen - 12] = '\0';
        snprintf(m->arg, sizeof(m->arg), "%s", m->cmd);
        snprintf(m->cmd, sizeof(m->cmd), "error");
        snprintf(m->sub, sizeof(m->sub), "header fail");
    }
}

typedef struct { const char *req; const char *img; int op; } ReqMap;
static const ReqMap req_map[] = {
    { "BL1",  "BL1",  0 },
    { "DPM",  "DPM",  0 },
    { "EPBL", "PBL",  0 },
    { "EPBB", "PBL",  2 },
    { "BL2",  "BL2",  1 },
    { "BL2B", "BL2",  2 },
    { "GSA1", "GSA",  0 },
    { "ABL",  "ABL",  1 },
    { "ABLB", "ABL",  2 },
    { "TZSW", "TZSW", 1 },
    { "TZSB", "TZSW", 2 },
    { "LDFW", "LDFW", 1 },
    { "LDFB", "LDFW", 2 },
    { "BL31", "BL31", 1 },
    { "BL3B", "BL31", 2 },
    { "GCF",  "GCF",  1 },
    { "GCFB", "GCF",  2 },
    { "GSAF", "GSAF", 0 },
    { NULL, NULL, 0 }
};

static bool send_image(int fd, const Image *img, int op)
{
    const uint8_t *data = img->data;
    size_t         len  = img->size;
    if (op == 1) {
        len = (len < HEADER_SIZE) ? len : HEADER_SIZE;
    } else if (op == 2) {
        if (len <= HEADER_SIZE) {
            fprintf(stderr, "[!] Image '%s' too small for body split\n", img->name);
            return false;
        }
        data += HEADER_SIZE;
        len  -= HEADER_SIZE;
    }
    return write_dnw(fd, data, len);
}

static void handle_request(int fd, const char *request)
{
    char req[64];
    strncpy(req, request, sizeof(req) - 1);
    req[sizeof(req)-1] = '\0';
    for (int i = 0; req[i]; i++)
        if (req[i] >= 'a' && req[i] <= 'z') req[i] -= 32;

    for (int i = 0; req_map[i].req; i++) {
        if (strcmp(req_map[i].req, req) != 0) continue;
        Image *img = find_image(req_map[i].img);
        if (!img) {
            fprintf(stderr, "[!] No image loaded for '%s' (need '%s')\n", req, req_map[i].img);
            return;
        }
        int op = req_map[i].op;
        printf("[>] Sending %s (op=%d, %zu bytes)\n", req, op, img->size);
        if (send_image(fd, img, op))
            printf("[+] Sent %s\n", req);
        else
            fprintf(stderr, "[-] Failed to send %s\n", req);
        return;
    }
    fprintf(stderr, "[!] Unknown image request: %s\n", req);
}

static void run(int fd)
{
    char line[512];
    char request[64] = "";
    bool upload      = false;

    ssize_t _ign = write(fd, "\n", 1); (void)_ign;

    printf("[*] Waiting for device messages...\n");

    while (1) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0) { perror("read_line"); break; }
        if (n == 0) continue;

        printf("[<] %s\n", line);

        Msg m;
        parse_msg(line, &m);

        if (strcmp(m.cmd, "exynos_usb_booting") == 0) {
            printf("[*] Device identified: %s\n", m.dev);

        } else if (strcmp(m.cmd, "eub") == 0) {

            // upper-case the argument
            char bl[64];
            strncpy(bl, m.arg, sizeof(bl) - 1);
            bl[sizeof(bl)-1] = '\0';
            for (int i = 0; bl[i]; i++)
                if (bl[i] >= 'a' && bl[i] <= 'z') bl[i] -= 32;

            if (strcmp(m.sub, "req") == 0) {
                if (strcmp(request, bl) == 0) {
                    // Exynos 9840 re-requests same stage instead of sending ack, may be the same on other chips.
                    printf("[~] Duplicate req for %s — treating as ACK, re-arming\n", bl);
                    upload = true;
                    continue;
                }
                printf("[*] Requested %s\n", bl);
                strncpy(request, bl, sizeof(request) - 1);
                request[sizeof(request)-1] = '\0';
                upload = true;

            } else if (strcmp(m.sub, "ack") == 0) {
                printf("[+] ACK for %s\n", bl);
                upload = true;  // re-arm: device ready for next C

            } else if (strcmp(m.sub, "nak") == 0) {
                fprintf(stderr, "[-] NAK for %s\n", bl);
            }

        } else if (m.cmd[0] == 'C' && m.cmd[1] == '\0') {
            if (!upload) {
                printf("[~] C signal but not ready to upload\n");
                continue;
            }
            upload = false;
            printf("[*] C signal — sending %s\n", request);
            handle_request(fd, request);
            if (debug_raw) {
                // Raw byte dump — see what the device sends immediately after
                uint8_t raw[128];
                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
                    ssize_t nr = read(fd, raw, sizeof(raw));
                    if (nr > 0) {
                        printf("[RAW] %zd bytes: ", nr);
                        for (ssize_t i = 0; i < nr; i++) printf("%02x ", raw[i]);
                        printf("\n");
                        // also print as string if printable
                        printf("[RAW] ascii: ");
                        for (ssize_t i = 0; i < nr; i++)
                            printf("%c", (raw[i] >= 0x20 && raw[i] < 0x7f) ? raw[i] : '.');
                        printf("\n");
                    }
                } else {
                    printf("[RAW] no response within 2s\n");
                }
            }

        } else if (strcmp(m.cmd, "irom_booting_failure") == 0) {
            fprintf(stderr, "[!] iROM booting failure: %s\n", m.dev);

        } else if (strcmp(m.cmd, "error") == 0) {
            fprintf(stderr, "[!] Error (%s): %s\n", m.sub, m.arg);

        } else if (strstr(line, "Ready to rx") != NULL) {
            printf("[~] %s\n", line);

        } else if (strstr(line, "rx done") != NULL || strstr(line, "Header pass") != NULL) {
            const char *p = line;
            const char *markers[] = { "rx done", "Header pass" };
            while (*p) {
                const char *best = NULL;
                size_t best_len = 0;
                for (int i = 0; i < 2; i++) {
                    const char *hit = strstr(p, markers[i]);
                    if (hit && (!best || hit < best)) {
                        best = hit;
                        best_len = strlen(markers[i]);
                    }
                }
                if (!best) {
                    printf("[~] %s\n", p);
                    break;
                }
                size_t seg_len = (size_t)(best - p) + best_len;
                printf("[~] %.*s\n", (int)seg_len, p);
                p = best + best_len;
            }

        } else {
            printf("[?] Unhandled: %s\n", line);
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <serial_device>\n"
        "\nOptions:\n"
        "  -s <dir>        Source directory for images (default: sources)\n"
        "  --bl1  <file>   BL1 image\n"
        "  --dpm  <file>   DPM image (zeroed 4KB if omitted)\n"
        "  --pbl  <file>   PBL/EPBL image\n"
        "  --bl2  <file>   BL2 image\n"
        "  --gsa  <file>   GSA image\n"
        "  --abl  <file>   ABL image\n"
        "  --tzsw <file>   TZSW image\n"
        "  --ldfw <file>   LDFW image\n"
        "  --bl31 <file>   BL31 image\n"
        "  --gcf  <file>   GCF image (optional)\n"
        "  --gsaf <file>   GSAF image (optional)\n"
        "  -h              Show this help\n"
        "  --debug         Dump raw bytes received after each send\n"
        "\nExample:\n"
        "  sudo %s -s ./sources /dev/ttyACM0\n", prog, prog);
}

static const char *get_arg(int argc, char **argv, const char *key, const char *shortkey)
{
    for (int i = 1; i < argc - 1; i++)
        if ((key      && strcmp(argv[i], key)      == 0) ||
            (shortkey && strcmp(argv[i], shortkey) == 0))
            return argv[i + 1];
    return NULL;
}

static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0) return true;
    return false;
}

static void register_image(const char *name, const char *src_dir, const char *filename)
{
    if (!filename || n_images >= MAX_IMAGES) return;
    size_t   sz   = 0;
    uint8_t *data = load_image(src_dir, filename, &sz);
    if (!data) {
        fprintf(stderr, "[~] Could not load '%s' for %s (optional or missing)\n", filename, name);
        return;
    }
    images[n_images].name = name;
    images[n_images].path = strdup(filename);
    images[n_images].data = data;
    images[n_images].size = sz;
    printf("[*] Loaded %-6s: %s (%zu bytes)\n", name, filename, sz);
    n_images++;
}

int main(int argc, char **argv)
{
    if (has_flag(argc, argv, "-h") || argc < 2) { usage(argv[0]); return 0; }
    if (has_flag(argc, argv, "--debug")) debug_raw = true;

    const char *serial_dev = argv[argc - 1];
    const char *src_dir    = get_arg(argc, argv, "-s", NULL);
    if (!src_dir) src_dir  = "sources";

    const char *f_bl1  = get_arg(argc, argv, "--bl1",  NULL) ?: "bl1.img";
    const char *f_dpm  = get_arg(argc, argv, "--dpm",  NULL);
    const char *f_pbl  = get_arg(argc, argv, "--pbl",  NULL) ?: "pbl.img";
    const char *f_bl2  = get_arg(argc, argv, "--bl2",  NULL) ?: "bl2.img";
    const char *f_gsa  = get_arg(argc, argv, "--gsa",  NULL) ?: "gsa.img";
    const char *f_abl  = get_arg(argc, argv, "--abl",  NULL) ?: "abl.img";
    const char *f_tzsw = get_arg(argc, argv, "--tzsw", NULL) ?: "tzsw.img";
    const char *f_ldfw = get_arg(argc, argv, "--ldfw", NULL) ?: "ldfw.img";
    const char *f_bl31 = get_arg(argc, argv, "--bl31", NULL) ?: "bl31.img";
    const char *f_gcf  = get_arg(argc, argv, "--gcf",  NULL) ?: "gcf.img";
    const char *f_gsaf = get_arg(argc, argv, "--gsaf", NULL) ?: "gsaf.img";

    printf("SETool - Exynos EUB/EDL Downloader\n\n");

    register_image("BL1",  src_dir, f_bl1);
    register_image("PBL",  src_dir, f_pbl);
    register_image("BL2",  src_dir, f_bl2);
    register_image("GSA",  src_dir, f_gsa);
    register_image("ABL",  src_dir, f_abl);
    register_image("TZSW", src_dir, f_tzsw);
    register_image("LDFW", src_dir, f_ldfw);
    register_image("BL31", src_dir, f_bl31);
    register_image("GCF",  src_dir, f_gcf);
    register_image("GSAF", src_dir, f_gsaf);

    if (n_images < MAX_IMAGES) {
        images[n_images].name = "DPM";
        images[n_images].path = strdup(f_dpm ? f_dpm : "(zeroed)");
        images[n_images].size = 4096;
        images[n_images].data = calloc(1, 4096);
        if (f_dpm) {
            size_t sz = 0;
            uint8_t *data = load_image(src_dir, f_dpm, &sz);
            if (data) { free(images[n_images].data); images[n_images].data = data; images[n_images].size = sz; }
        }
        printf("[*] Loaded %-6s: %s (%zu bytes)\n", "DPM", images[n_images].path, images[n_images].size);
        n_images++;
    }

    printf("\n[*] Opening %s ...\n", serial_dev);
    int fd = open_serial(serial_dev);
    if (fd < 0) return 1;
    printf("[+] Connected\n\n");

    run(fd);
    close(fd);

    for (int i = 0; i < n_images; i++) { free(images[i].data); free(images[i].path); }
    return 0;
}