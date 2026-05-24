#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define MAX_FILES 32
#define MAX_TOTAL_SIZE (200UL * 1024 * 1024)
#define HEADER_SIZE 10
#define DEFAULT_OUTPUT "a.sau"

static void print_usage(void) {
    fprintf(stderr, "Kullanim:\n");
    fprintf(stderr, "  tarsau -b dosya1 dosya2 ... [-o arsiv.sau]\n");
    fprintf(stderr, "  tarsau -a arsiv.sau [dizin]\n");
}

static int is_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\0') {
            fclose(f);
            return 0;
        }
        unsigned char uc = (unsigned char)ch;
        if (uc > 127) {
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return 1;
}

static long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

static mode_t get_permissions(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return st.st_mode & 0777;
}

static int archive_files(int file_count, char *files[], const char *output) {
    if (file_count == 0) {
        fprintf(stderr, "Hata: Arsivlenecek dosya belirtilmedi.\n");
        return 1;
    }

    if (file_count > MAX_FILES) {
        fprintf(stderr, "Hata: En fazla %d dosya arsivlenebilir.\n", MAX_FILES);
        return 1;
    }

    long sizes[MAX_FILES];
    mode_t perms[MAX_FILES];
    unsigned long total_size = 0;

    for (int i = 0; i < file_count; i++) {
        sizes[i] = get_file_size(files[i]);
        if (sizes[i] < 0) {
            fprintf(stderr, "Hata: '%s' dosyasi acilamadi.\n", files[i]);
            return 1;
        }

        if (!is_text_file(files[i])) {
            fprintf(stderr, "%s giris dosyasinin formati uyumsuzdur!\n", files[i]);
            return 1;
        }

        perms[i] = get_permissions(files[i]);
        total_size += (unsigned long)sizes[i];
    }

    if (total_size > MAX_TOTAL_SIZE) {
        fprintf(stderr, "Hata: Toplam dosya boyutu 200 MB sinirini asiyor.\n");
        return 1;
    }

    char org_section[65536];
    int org_len = 0;

    for (int i = 0; i < file_count; i++) {
        int written = snprintf(org_section + org_len, sizeof(org_section) - org_len,
                               "|%s,%o,%ld|", files[i], perms[i], sizes[i]);
        if (written < 0 || org_len + written >= (int)sizeof(org_section)) {
            fprintf(stderr, "Hata: Organizasyon bolumu cok buyuk.\n");
            return 1;
        }
        org_len += written;
    }

    FILE *out = fopen(output, "w");
    if (!out) {
        fprintf(stderr, "Hata: '%s' cikti dosyasi olusturulamadi.\n", output);
        return 1;
    }

    char header[HEADER_SIZE + 1];
    snprintf(header, sizeof(header), "%010d", org_len);
    if (fwrite(header, 1, HEADER_SIZE, out) != HEADER_SIZE) {
        fprintf(stderr, "Hata: Header yazilamadi.\n");
        fclose(out);
        return 1;
    }

    if (fwrite(org_section, 1, org_len, out) != (size_t)org_len) {
        fprintf(stderr, "Hata: Organizasyon bolumu yazilamadi.\n");
        fclose(out);
        return 1;
    }

    for (int i = 0; i < file_count; i++) {
        FILE *in = fopen(files[i], "r");
        if (!in) {
            fprintf(stderr, "Hata: '%s' dosyasi okunamadi.\n", files[i]);
            fclose(out);
            return 1;
        }

        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                fprintf(stderr, "Hata: Dosya yazma hatasi.\n");
                fclose(in);
                fclose(out);
                return 1;
            }
        }
        fclose(in);
    }

    fclose(out);
    printf("Dosyalar birlestirildi.\n");
    return 0;
}

static int extract_files(const char *archive, const char *dest_dir) {
    FILE *f = fopen(archive, "r");
    if (!f) {
        fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
        return 1;
    }

    char header[HEADER_SIZE + 1];
    if (fread(header, 1, HEADER_SIZE, f) != HEADER_SIZE) {
        fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
        fclose(f);
        return 1;
    }
    header[HEADER_SIZE] = '\0';

    int org_len = atoi(header);
    if (org_len <= 0) {
        fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
        fclose(f);
        return 1;
    }

    char *org_section = malloc(org_len + 1);
    if (!org_section) {
        fprintf(stderr, "Hata: Bellek ayrilamadi.\n");
        fclose(f);
        return 1;
    }

    if (fread(org_section, 1, org_len, f) != (size_t)org_len) {
        fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
        free(org_section);
        fclose(f);
        return 1;
    }
    org_section[org_len] = '\0';

    char filenames[MAX_FILES][256];
    mode_t perms[MAX_FILES];
    long sizes[MAX_FILES];
    int file_count = 0;

    char *p = org_section;
    while (*p && file_count < MAX_FILES) {
        if (*p == '|') p++;
        if (*p == '\0') break;

        char *end = strchr(p, '|');
        if (!end) break;

        char record[512];
        int rlen = end - p;
        if (rlen <= 0 || rlen >= (int)sizeof(record)) break;
        memcpy(record, p, rlen);
        record[rlen] = '\0';

        char *comma1 = strchr(record, ',');
        if (!comma1) {
            fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
            free(org_section);
            fclose(f);
            return 1;
        }
        *comma1 = '\0';

        char *comma2 = strchr(comma1 + 1, ',');
        if (!comma2) {
            fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
            free(org_section);
            fclose(f);
            return 1;
        }
        *comma2 = '\0';

        strncpy(filenames[file_count], record, sizeof(filenames[file_count]) - 1);
        filenames[file_count][sizeof(filenames[file_count]) - 1] = '\0';
        perms[file_count] = (mode_t)strtol(comma1 + 1, NULL, 8);
        sizes[file_count] = atol(comma2 + 1);

        file_count++;
        p = end + 1;
    }

    free(org_section);

    if (file_count == 0) {
        fprintf(stderr, "Arsiv dosyasi uygunsuz veya bozuk!\n");
        fclose(f);
        return 1;
    }

    if (dest_dir && strlen(dest_dir) > 0) {
        struct stat st;
        if (stat(dest_dir, &st) != 0) {
            if (mkdir(dest_dir, 0755) != 0) {
                fprintf(stderr, "Hata: '%s' dizini olusturulamadi.\n", dest_dir);
                fclose(f);
                return 1;
            }
        }
    }

    for (int i = 0; i < file_count; i++) {
        char filepath[512];
        if (dest_dir && strlen(dest_dir) > 0) {
            snprintf(filepath, sizeof(filepath), "%s/%s", dest_dir, filenames[i]);
        } else {
            strncpy(filepath, filenames[i], sizeof(filepath) - 1);
            filepath[sizeof(filepath) - 1] = '\0';
        }

        FILE *out = fopen(filepath, "w");
        if (!out) {
            fprintf(stderr, "Hata: '%s' dosyasi olusturulamadi.\n", filepath);
            fclose(f);
            return 1;
        }

        long remaining = sizes[i];
        char buf[8192];
        while (remaining > 0) {
            size_t to_read = (remaining < (long)sizeof(buf)) ? (size_t)remaining : sizeof(buf);
            size_t n = fread(buf, 1, to_read, f);
            if (n == 0) {
                fprintf(stderr, "Hata: Arsiv dosyasi beklenenden kisa.\n");
                fclose(out);
                fclose(f);
                return 1;
            }
            if (fwrite(buf, 1, n, out) != n) {
                fprintf(stderr, "Hata: Dosya yazma hatasi.\n");
                fclose(out);
                fclose(f);
                return 1;
            }
            remaining -= (long)n;
        }

        fclose(out);

        if (chmod(filepath, perms[i]) != 0) {
            fprintf(stderr, "Uyari: '%s' dosyasinin izinleri ayarlanamadi.\n", filepath);
        }
    }

    fclose(f);

    if (dest_dir && strlen(dest_dir) > 0) {
        printf("%s dizininde ", dest_dir);
    }

    for (int i = 0; i < file_count; i++) {
        if (i > 0) printf(", ");
        printf("%s", filenames[i]);
    }
    printf(" dosyalari acildi.\n");

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "-b") == 0) {
        char *files[MAX_FILES];
        int file_count = 0;
        const char *output = DEFAULT_OUTPUT;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 < argc) {
                    output = argv[++i];
                } else {
                    fprintf(stderr, "Hata: -o parametresinden sonra dosya adi belirtilmedi.\n");
                    return 1;
                }
            } else {
                if (file_count >= MAX_FILES) {
                    fprintf(stderr, "Hata: En fazla %d dosya arsivlenebilir.\n", MAX_FILES);
                    return 1;
                }
                files[file_count++] = argv[i];
            }
        }

        return archive_files(file_count, files, output);

    } else if (strcmp(argv[1], "-a") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Hata: Arsiv dosyasi belirtilmedi.\n");
            print_usage();
            return 1;
        }

        const char *archive = argv[2];
        const char *dest_dir = NULL;

        if (argc >= 4) {
            dest_dir = argv[3];
        }

        return extract_files(archive, dest_dir);

    } else {
        fprintf(stderr, "Hata: Bilinmeyen parametre '%s'\n", argv[1]);
        print_usage();
        return 1;
    }
}
