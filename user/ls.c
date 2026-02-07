#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static int g_errors = 0;

static void print_entry(const char* name, const struct stat* st) {
    char type = '-';
    if ((st->st_mode & S_IFMT) == S_IFDIR) type = 'd';
    printf("%s %c %lu\n", name, type, (unsigned long)st->st_size);
}

static void list_dir(const char* path, int show_header) {
    DIR* dir = opendir(path);
    if (!dir) {
        printf("ls: cannot access %s (%d)\n", path, errno);
        g_errors++;
        return;
    }
    if (show_header) {
        printf("%s:\n", path);
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        char type = '-';
        unsigned long size = 0;
        
        // Build full path for stat
        char fullpath[512];
        size_t plen = strlen(path);
        if (plen > 0 && path[plen-1] == '/') {
            snprintf(fullpath, sizeof(fullpath), "%s%s", path, ent->d_name);
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
        }
        
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            if ((st.st_mode & S_IFMT) == S_IFDIR) type = 'd';
            size = (unsigned long)st.st_size;
        } else if (ent->d_type == DT_DIR) {
            type = 'd';
        }
        printf("%s %c %lu\n", ent->d_name, type, size);
    }
    closedir(dir);
}

static void list_path(const char* path, int show_header) {
    DIR* dir = opendir(path);
    if (dir) {
        closedir(dir);
        list_dir(path, show_header);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("ls: cannot access %s (%d)\n", path, errno);
        g_errors++;
        return;
    }
    print_entry(path, &st);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        list_path(".", 0);
        return g_errors > 0 ? 1 : 0;
    }
    for (int i = 1; i < argc; ++i) {
        list_path(argv[i], argc > 2);
    }
    return g_errors > 0 ? 1 : 0;
}
