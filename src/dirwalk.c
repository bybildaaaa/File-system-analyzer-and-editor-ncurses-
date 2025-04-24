#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <ncurses.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>

#define MAX_FILES 20000
#define MAX_PATH 4096
#define MAX_UNDO 100
#define MAX_DIR_CONTENTS 1000
#define MAX_VIEW_CONTENT 1024

// Глобальные настройки
int sort_by_size = 0;
int show_links = 0;
int show_dirs = 0;
int show_files = 0;

// Структура для хранения информации о файле
typedef struct {
    char *full_path; // Полный путь для операций
    char *display_path; // Относительный путь для отображения
    off_t size;
    mode_t mode;
    time_t mtime;
} FileInfo;

// Структура для хранения содержимого директории для undo
typedef struct {
    char *path;
    char *content;
    int is_dir;
} DirContent;

// Структура для undo
typedef enum { ACTION_DELETE, ACTION_CREATE, ACTION_RENAME, ACTION_CHMOD, ACTION_EDIT, ACTION_MOVE } ActionType;
typedef struct {
    ActionType type;
    char *path;
    char *old_path; // Для переименования и перемещения
    mode_t old_mode; // Для chmod
    char *content; // Для редактирования
    DirContent *dir_contents; // Для содержимого директории
    int dir_content_count; // Количество элементов в директории
} UndoAction;

UndoAction undo_stack[MAX_UNDO];
int undo_count = 0;

// Сравнение для сортировки
int compare_files(const void *a, const void *b) {
    FileInfo *fa = *(FileInfo **)a;
    FileInfo *fb = *(FileInfo **)b;
    if (sort_by_size) {
        if (fb->size != fa->size) {
            return (fb->size > fa->size) ? 1 : -1;
        }
    }
    return strcoll(fa->display_path, fb->display_path); // Сортировка по отображаемому пути
}

// Проверка соответствия типа файла фильтру
int match_type(struct stat *sb) {
    if (!show_links && !show_dirs && !show_files) {
        return 1; // Если флаги не заданы, показываем все
    }
    return (show_links && S_ISLNK(sb->st_mode)) ||
           (show_dirs && S_ISDIR(sb->st_mode)) ||
           (show_files && S_ISREG(sb->st_mode));
}

// Очистка пути
char *clean_path(const char *path, const char *base, char **full_path) {
    char *cleaned = malloc(MAX_PATH);
    if (!cleaned) {
        perror("malloc");
        return NULL;
    }
    
    *full_path = malloc(MAX_PATH);
    if (!*full_path) {
        free(cleaned);
        perror("malloc");
        return NULL;
    }

    // Формируем полный путь
    if (path[0] == '.' && path[1] == '/') {
        snprintf(*full_path, MAX_PATH, "%s%s", base, path + 1);
    } else {
        strncpy(*full_path, path, MAX_PATH - 1);
        (*full_path)[MAX_PATH - 1] = '\0';
    }

    // Удаляем двойные слэши из полного пути
    char *p = *full_path;
    while (*p) {
        if (*p == '/' && *(p+1) == '/') {
            memmove(p, p+1, strlen(p));
        } else {
            p++;
        }
    }

    // Формируем относительный путь
    size_t base_len = strlen(base);
    if (strncmp(*full_path, base, base_len) == 0 && (*full_path)[base_len] == '/') {
        snprintf(cleaned, MAX_PATH, ".%s", *full_path + base_len);
    } else if (strcmp(*full_path, base) == 0) {
        strcpy(cleaned, ".");
    } else {
        strncpy(cleaned, *full_path, MAX_PATH - 1);
        cleaned[MAX_PATH - 1] = '\0';
    }

    return cleaned;
}

// Проверка существования директории
int directory_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

// Рекурсивное сохранение содержимого директории для undo
int save_directory_contents(const char *path, DirContent **contents, int *content_count) {
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir");
        return -1;
    }

    struct dirent *dir;
    struct stat stat_block;
    char fullpath[MAX_PATH];

    while ((dir = readdir(d)) && *content_count < MAX_DIR_CONTENTS) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir->d_name);

        if (lstat(fullpath, &stat_block) == -1) {
            perror("lstat");
            continue;
        }

        contents[*content_count] = malloc(sizeof(DirContent));
        if (!contents[*content_count]) {
            perror("malloc");
            closedir(d);
            return -1;
        }
        contents[*content_count]->path = strdup(fullpath);
        contents[*content_count]->content = NULL;
        contents[*content_count]->is_dir = S_ISDIR(stat_block.st_mode);

        if (S_ISREG(stat_block.st_mode)) {
            FILE *file = fopen(fullpath, "r");
            if (file) {
                fseek(file, 0, SEEK_END);
                long size = ftell(file);
                fseek(file, 0, SEEK_SET);
                contents[*content_count]->content = malloc(size + 1);
                if (contents[*content_count]->content) {
                    fread(contents[*content_count]->content, 1, size, file);
                    contents[*content_count]->content[size] = '\0';
                }
                fclose(file);
            }
        } else if (S_ISDIR(stat_block.st_mode)) {
            save_directory_contents(fullpath, contents, content_count);
        }

        (*content_count)++;
    }
    closedir(d);
    return 0;
}

// Рекурсивный обход директории
int dirwalk(const char *path, FileInfo **files, int *count, const char *base) {
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir");
        return -1;
    }

    struct dirent *dir;
    struct stat stat_block;
    char fullpath[MAX_PATH];

    while ((dir = readdir(d))) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir->d_name);

        if (lstat(fullpath, &stat_block) == -1) {
            perror("lstat");
            continue;
        }

        if (match_type(&stat_block)) {
            if (*count >= MAX_FILES) {
                fprintf(stderr, "Too many files\n");
                closedir(d);
                return -1;
            }
            files[*count] = malloc(sizeof(FileInfo));
            if (!files[*count]) {
                perror("malloc");
                closedir(d);
                return -1;
            }
            files[*count]->full_path = NULL;
            files[*count]->display_path = clean_path(fullpath, base, &files[*count]->full_path);
            if (!files[*count]->display_path || !files[*count]->full_path) {
                free(files[*count]->display_path);
                free(files[*count]->full_path);
                free(files[*count]);
                closedir(d);
                return -1;
            }
            files[*count]->size = stat_block.st_size;
            files[*count]->mode = stat_block.st_mode;
            files[*count]->mtime = stat_block.st_mtime;
            (*count)++;
        }

        if (S_ISDIR(stat_block.st_mode)) {
            dirwalk(fullpath, files, count, base);
        }
    }
    closedir(d);
    return 0;
}

// Копирование файла
int copy_file(const char *src, const char *dst) {
    FILE *source = fopen(src, "rb");
    FILE *dest = fopen(dst, "wb");
    if (!source || !dest) {
        perror("fopen");
        if (source) fclose(source);
        if (dest) fclose(dest);
        return -1;
    }

    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        fwrite(buffer, 1, bytes, dest);
    }

    fclose(source);
    fclose(dest);
    return 0;
}

// Форматирование размера файла
char *format_size(off_t size) {
    static char buf[32];
    if (size > 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    } else if (size > 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%ld B", size);
    }
    return buf;
}

// Рекурсивное удаление директории
int remove_directory(const char *path, DirContent **contents, int *content_count) {
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir");
        return -1;
    }

    struct dirent *dir;
    struct stat stat_block;
    char fullpath[MAX_PATH];

    while ((dir = readdir(d))) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dir->d_name);

        if (lstat(fullpath, &stat_block) == -1) {
            perror("lstat");
            continue;
        }

        if (S_ISDIR(stat_block.st_mode)) {
            if (remove_directory(fullpath, contents, content_count) == -1) {
                closedir(d);
                return -1;
            }
        } else {
            if (unlink(fullpath) == -1) {
                perror("unlink");
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);

    if (rmdir(path) == -1) {
        perror("rmdir");
        return -1;
    }
    return 0;
}

// Просмотр содержимого файла
int view_file(const char *path, WINDOW *view_win) {
    FILE *file = fopen(path, "r");
    if (!file) {
        wclear(view_win);
        box(view_win, 0, 0);
        mvwprintw(view_win, 1, 1, "Error: Cannot open file");
        wrefresh(view_win);
        getch();
        return -1;
    }

    char content[MAX_VIEW_CONTENT + 1];
    size_t bytes = fread(content, 1, MAX_VIEW_CONTENT, file);
    content[bytes] = '\0';
    fclose(file);

    wclear(view_win);
    box(view_win, 0, 0);
    mvwprintw(view_win, 1, 1, "File content (press any key to exit):");
    mvwprintw(view_win, 2, 1, "%s", content);
    wrefresh(view_win);
    getch();
    return 0;
}

// Редактирование содержимого файла
int edit_file(const char *path, WINDOW *dialog_win) {
    char content[1024] = "";
    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "Enter new content: ");
    wrefresh(dialog_win);
    echo();
    wgetnstr(dialog_win, content, sizeof(content));
    noecho();

    // Сохраняем старое содержимое для undo
    FILE *file = fopen(path, "r");
    char *old_content = NULL;
    if (file) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        old_content = malloc(size + 1);
        if (old_content) {
            fread(old_content, 1, size, file);
            old_content[size] = '\0';
        }
        fclose(file);
    }

    // Записываем новое содержимое
    file = fopen(path, "w");
    if (!file) {
        perror("fopen");
        free(old_content);
        return -1;
    }
    fprintf(file, "%s", content);
    fclose(file);

    // Добавляем в стек undo
    if (undo_count < MAX_UNDO) {
        undo_stack[undo_count].type = ACTION_EDIT;
        undo_stack[undo_count].path = strdup(path);
        undo_stack[undo_count].content = old_content ? old_content : strdup("");
        undo_stack[undo_count].dir_contents = NULL;
        undo_stack[undo_count].dir_content_count = 0;
        undo_count++;
    } else {
        free(old_content);
    }

    return 0;
}

// Переименование файла
int rename_file(const char *old_path, WINDOW *dialog_win, const char *base_path) {
    char new_name[256];
    char new_path[MAX_PATH];
    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "New name: ");
    wrefresh(dialog_win);
    echo();
    wgetnstr(dialog_win, new_name, sizeof(new_name));
    noecho();

    snprintf(new_path, sizeof(new_path), "%s/%s", base_path, new_name);

    // Проверка существования нового имени
    if (access(new_path, F_OK) == 0) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: Name already exists");
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    if (rename(old_path, new_path) == -1) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    // Добавляем в стек undo
    if (undo_count < MAX_UNDO) {
        undo_stack[undo_count].type = ACTION_RENAME;
        undo_stack[undo_count].path = strdup(new_path);
        undo_stack[undo_count].old_path = strdup(old_path);
        undo_stack[undo_count].content = NULL;
        undo_stack[undo_count].dir_contents = NULL;
        undo_stack[undo_count].dir_content_count = 0;
        undo_count++;
    }

    return 0;
}

// Перемещение файла
int move_file(const char *old_path, WINDOW *dialog_win) {
    char new_path[MAX_PATH];
    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "New full path: ");
    wrefresh(dialog_win);
    echo();
    wgetnstr(dialog_win, new_path, sizeof(new_path));
    noecho();

    // Проверка существования директории
    char *last_slash = strrchr(new_path, '/');
    if (last_slash) {
        char dir_path[MAX_PATH];
        strncpy(dir_path, new_path, last_slash - new_path);
        dir_path[last_slash - new_path] = '\0';
        if (!directory_exists(dir_path)) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: Directory does not exist");
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
    }

    if (rename(old_path, new_path) == -1) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    // Добавляем в стек undo
    if (undo_count < MAX_UNDO) {
        undo_stack[undo_count].type = ACTION_MOVE;
        undo_stack[undo_count].path = strdup(new_path);
        undo_stack[undo_count].old_path = strdup(old_path);
        undo_stack[undo_count].content = NULL;
        undo_stack[undo_count].dir_contents = NULL;
        undo_stack[undo_count].dir_content_count = 0;
        undo_count++;
    }

    return 0;
}

// Восстановление содержимого директории
int restore_directory_contents(DirContent *contents, int content_count) {
    for (int i = 0; i < content_count; i++) {
        if (contents[i].is_dir) {
            mkdir(contents[i].path, 0755);
        } else {
            FILE *file = fopen(contents[i].path, "w");
            if (file && contents[i].content) {
                fprintf(file, "%s", contents[i].content);
                fclose(file);
            }
        }
    }
    return 0;
}

// Отмена последнего действия
int undo_last_action(FileInfo **files, int *count, const char *base_path) {
    if (undo_count == 0) {
        return -1;
    }

    UndoAction *action = &undo_stack[undo_count - 1];
    
    switch (action->type) {
        case ACTION_DELETE:
            // Восстановление удаленного файла/директории
            if (action->content) {
                FILE *file = fopen(action->path, "w");
                if (file) {
                    fprintf(file, "%s", action->content);
                    fclose(file);
                }
            } else {
                mkdir(action->path, 0755);
                // Восстановление содержимого директории
                if (action->dir_contents && action->dir_content_count > 0) {
                    restore_directory_contents(action->dir_contents, action->dir_content_count);
                }
            }
            break;
        case ACTION_CREATE:
            // Удаление созданного объекта
            if (access(action->path, F_OK) == 0) {
                struct stat st;
                lstat(action->path, &st);
                if (S_ISDIR(st.st_mode)) {
                    DirContent *dummy_contents[MAX_DIR_CONTENTS];
                    int dummy_count = 0;
                    remove_directory(action->path, dummy_contents, &dummy_count);
                } else {
                    unlink(action->path);
                }
            }
            break;
        case ACTION_RENAME:
        case ACTION_MOVE:
            // Возврат старого имени/пути
            rename(action->path, action->old_path);
            break;
        case ACTION_CHMOD:
            // Восстановление старых прав
            chmod(action->path, action->old_mode);
            break;
        case ACTION_EDIT:
            // Восстановление старого содержимого
            FILE *file = fopen(action->path, "w");
            if (file) {
                fprintf(file, "%s", action->content);
                fclose(file);
            }
            break;
    }

    // Очистка действия
    free(action->path);
    if (action->old_path) free(action->old_path);
    if (action->content) free(action->content);
    if (action->dir_contents) {
        for (int i = 0; i < action->dir_content_count; i++) {
            free(action->dir_contents[i].path);
            if (action->dir_contents[i].content) free(action->dir_contents[i].content);
        }
        free(action->dir_contents);
    }
    undo_count--;

    // Пересобираем список файлов
    for (int i = 0; i < *count; i++) {
        free(files[i]->full_path);
        free(files[i]->display_path);
        free(files[i]);
    }
    *count = 0;
    dirwalk(base_path, files, count, base_path);
    qsort(files, *count, sizeof(FileInfo *), compare_files);

    return 0;
}

// Инициализация ncurses
void init_ncurses() {
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);  // Папки
    init_pair(2, COLOR_GREEN, COLOR_BLACK); // Файлы
    init_pair(3, COLOR_YELLOW, COLOR_BLACK); // Ссылки
}

// Отображение списка файлов
void display_files(WINDOW *win, FileInfo **files, int count, int selected, int offset) {
    wclear(win);
    box(win, 0, 0);
    int max_y, max_x __attribute__((unused));
    getmaxyx(win, max_y, max_x);
    max_y -= 2; // Учитываем рамку

    for (int i = offset; i < count && i < offset + max_y; i++) {
        if (i == selected) {
            wattron(win, A_REVERSE);
        }
        if (S_ISDIR(files[i]->mode)) {
            wattron(win, COLOR_PAIR(1));
            mvwprintw(win, i - offset + 1, 1, "%s/", files[i]->display_path);
            wattroff(win, COLOR_PAIR(1));
        } else if (S_ISLNK(files[i]->mode)) {
            wattron(win, COLOR_PAIR(3));
            mvwprintw(win, i - offset + 1, 1, "%s", files[i]->display_path);
            wattroff(win, COLOR_PAIR(3));
        } else {
            wattron(win, COLOR_PAIR(2));
            mvwprintw(win, i - offset + 1, 1, "%s", files[i]->display_path);
            wattroff(win, COLOR_PAIR(2));
        }
        if (i == selected) {
            wattroff(win, A_REVERSE);
        }
    }
    wrefresh(win);
}

// Отображение информации о файле
void display_info(WINDOW *win, FileInfo *file) {
    wclear(win);
    box(win, 0, 0);
    if (!file) {
        wrefresh(win);
        return;
    }

    char *name = strrchr(file->display_path, '/') ? strrchr(file->display_path, '/') + 1 : file->display_path;
    char time_buf[26];
    ctime_r(&file->mtime, time_buf);
    time_buf[strlen(time_buf) - 1] = '\0'; // Удаляем \n

    mvwprintw(win, 1, 1, "Name: %s", name);
    mvwprintw(win, 2, 1, "Size: %s", format_size(file->size));
    mvwprintw(win, 3, 1, "Type: %s", S_ISDIR(file->mode) ? "Directory" : S_ISLNK(file->mode) ? "Link" : "File");
    mvwprintw(win, 4, 1, "Modified: %s", time_buf);
    mvwprintw(win, 5, 1, "Perm: %o", file->mode & 0777);
    wrefresh(win);
}

// Диалоговое окно для подтверждения
int confirm_dialog(WINDOW *win, const char *message) {
    wclear(win);
    box(win, 0, 0);
    mvwprintw(win, 1, 1, "%s [Y/N]", message);
    wrefresh(win);
    int ch;
    while ((ch = getch()) != 'y' && ch != 'n') {}
    wclear(win);
    wrefresh(win);
    return ch == 'y';
}

// Функция для изменения прав доступа
int change_permissions(const char *path, WINDOW *dialog_win) {
    // Проверка существования файла
    struct stat st;
    if (lstat(path, &st) == -1) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: Cannot stat file");
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    // Проверка на ссылку и поддержку lchmod
#ifndef HAVE_LCHMOD
    if (S_ISLNK(st.st_mode)) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: Link perms not supported");
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }
#endif

    char input[10];
    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "Perms (octal, 755): ");
    wrefresh(dialog_win);
    echo();
    wgetnstr(dialog_win, input, sizeof(input));
    noecho();

    // Проверка валидности прав
    mode_t new_mode;
    if (sscanf(input, "%o", &new_mode) != 1 || new_mode > 0777) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: Invalid permissions");
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    // Изменение прав
#ifdef HAVE_LCHMOD
    if (S_ISLNK(st.st_mode)) {
        if (lchmod(path, new_mode) == -1) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
    } else {
        if (chmod(path, new_mode) == -1) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
    }
#else
    if (chmod(path, new_mode) == -1) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }
#endif

    // Добавляем в стек undo
    if (undo_count < MAX_UNDO) {
        undo_stack[undo_count].type = ACTION_CHMOD;
        undo_stack[undo_count].path = strdup(path);
        undo_stack[undo_count].old_mode = st.st_mode;
        undo_stack[undo_count].content = NULL;
        undo_stack[undo_count].dir_contents = NULL;
        undo_stack[undo_count].dir_content_count = 0;
        undo_count++;
    }

    wclear(dialog_win);
    wrefresh(dialog_win);
    return 0;
}

// Функция для создания нового объекта
int create_object(const char *base_path, WINDOW *dialog_win) {
    char input[256];
    char fullpath[MAX_PATH];
    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "Name for file/dir/link: ");
    wrefresh(dialog_win);
    echo();
    wgetnstr(dialog_win, input, sizeof(input));
    noecho();

    snprintf(fullpath, sizeof(fullpath), "%s/%s", base_path, input);

    // Проверка существования имени
    if (access(fullpath, F_OK) == 0) {
        wclear(dialog_win);
        mvwprintw(dialog_win, 1, 1, "Error: Name already exists");
        wrefresh(dialog_win);
        getch();
        wclear(dialog_win);
        wrefresh(dialog_win);
        return -1;
    }

    wclear(dialog_win);
    box(dialog_win, 0, 0);
    mvwprintw(dialog_win, 1, 1, "[F]ile, [D]ir or [L]ink? ");
    wrefresh(dialog_win);
    int ch;
    while ((ch = getch()) != 'f' && ch != 'd' && ch != 'l') {}

    if (ch == 'f') {
        int fd = open(fullpath, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd == -1) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
        close(fd);
    } else if (ch == 'd') {
        if (mkdir(fullpath, 0755) == -1) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
    } else {
        char target[256];
        wclear(dialog_win);
        box(dialog_win, 0, 0);
        mvwprintw(dialog_win, 1, 1, "Link target: ");
        wrefresh(dialog_win);
        echo();
        wgetnstr(dialog_win, target, sizeof(target));
        noecho();
        if (symlink(target, fullpath) == -1) {
            wclear(dialog_win);
            mvwprintw(dialog_win, 1, 1, "Error: %s", strerror(errno));
            wrefresh(dialog_win);
            getch();
            wclear(dialog_win);
            wrefresh(dialog_win);
            return -1;
        }
    }

    // Добавляем в стек undo
    if (undo_count < MAX_UNDO) {
        undo_stack[undo_count].type = ACTION_CREATE;
        undo_stack[undo_count].path = strdup(fullpath);
        undo_stack[undo_count].content = NULL;
        undo_stack[undo_count].dir_contents = NULL;
        undo_stack[undo_count].dir_content_count = 0;
        undo_count++;
    }

    wclear(dialog_win);
    wrefresh(dialog_win);
    return 0;
}

int main(int argc, char *argv[]) {
    setlocale(LC_COLLATE, "");
    char *dir_path = NULL;
    char resolved_path[PATH_MAX];
    int opt;
    char flags[256] = "Used flags: ";

    // Обработка аргументов
    while ((opt = getopt(argc, argv, "sldf")) != -1) {
        switch (opt) {
            case 's':
                sort_by_size = 1;
                strcat(flags, "-s ");
                break;
            case 'l':
                show_links = 1;
                strcat(flags, "-l ");
                break;
            case 'd':
                show_dirs = 1;
                strcat(flags, "-d ");
                break;
            case 'f':
                show_files = 1;
                strcat(flags, "-f ");
                break;
            default:
                fprintf(stderr, "Usage: %s [-s (size)] [-l (links)] [-d (dirs)] [-f (files)] [directory]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Определение директории
    if (optind < argc) {
        // Проверяем указанную директорию
        if (realpath(argv[optind], resolved_path) == NULL) {
            fprintf(stderr, "Error: Cannot resolve path %s: %s\n", argv[optind], strerror(errno));
            exit(EXIT_FAILURE);
        }
        struct stat st;
        if (stat(resolved_path, &st) == -1 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Error: %s is not a directory\n", resolved_path);
            exit(EXIT_FAILURE);
        }
        dir_path = resolved_path;
    } else {
        // Используем текущую директорию
        if (getcwd(resolved_path, sizeof(resolved_path)) == NULL) {
            fprintf(stderr, "Error: Cannot get current directory: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        dir_path = resolved_path;
    }

    // Сбор файлов
    FileInfo *files[MAX_FILES];
    int count = 0;
    if (dirwalk(dir_path, files, &count, dir_path) != 0) {
        fprintf(stderr, "Failed to walk directory\n");
        return 1;
    }

    // Сортировка
    qsort(files, count, sizeof(FileInfo *), compare_files);

    // Инициализация ncurses
    init_ncurses();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    WINDOW *file_win = newwin(max_y - 10, max_x - 2, 1, 1);
    WINDOW *info_win = newwin(8, max_x - 2, max_y - 9, 1);
    WINDOW *dialog_win = newwin(3, 50, max_y / 2 - 1, max_x / 2 - 25);
    WINDOW *view_win = newwin(max_y - 4, max_x - 4, 2, 2);

    // Вывод флагов
    mvprintw(0, 1, "%s", flags);
    refresh();

    // Вывод инструкций
    mvprintw(max_y - 1, 1, "q:Quit Up/Dn:Nav c:Copy d:Del m:Chmod n:New e:Edit r:Ren p:Move u:Undo v:View");
    clrtoeol();
    refresh();

    int selected = 0, offset = 0;
    display_files(file_win, files, count, selected, offset);
    display_info(info_win, selected < count ? files[selected] : NULL);

    int ch;
    while ((ch = getch()) != 'q') {
        switch (ch) {
            case KEY_UP:
                if (selected > 0) {
                    selected--;
                    if (selected < offset) offset--;
                }
                break;
            case KEY_DOWN:
                if (selected < count - 1) {
                    selected++;
                    if (selected >= offset + (max_y - 12)) offset++;
                }
                break;
            case 'c':
                if (selected < count && S_ISREG(files[selected]->mode)) {
                    char dst_path[MAX_PATH];
                    snprintf(dst_path, sizeof(dst_path), "%s.copy", files[selected]->full_path);
                    if (confirm_dialog(dialog_win, "Copy file?")) {
                        if (copy_file(files[selected]->full_path, dst_path) == 0) {
                            mvprintw(max_y - 2, 1, "File copied to %s", dst_path);
                            // Обновляем список
                            for (int i = 0; i < count; i++) {
                                free(files[i]->full_path);
                                free(files[i]->display_path);
                                free(files[i]);
                            }
                            count = 0;
                            dirwalk(dir_path, files, &count, dir_path);
                            qsort(files, count, sizeof(FileInfo *), compare_files);
                        } else {
                            mvprintw(max_y - 2, 1, "Copy failed");
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'd':
                if (selected < count) {
                    if (confirm_dialog(dialog_win, S_ISDIR(files[selected]->mode) ? "Delete directory?" : S_ISLNK(files[selected]->mode) ? "Delete link?" : "Delete file?")) {
                        int success;
                        char *content = NULL;
                        DirContent *dir_contents[MAX_DIR_CONTENTS] = {0};
                        int dir_content_count = 0;

                        if (S_ISREG(files[selected]->mode)) {
                            FILE *file = fopen(files[selected]->full_path, "r");
                            if (file) {
                                fseek(file, 0, SEEK_END);
                                long size = ftell(file);
                                fseek(file, 0, SEEK_SET);
                                content = malloc(size + 1);
                                if (content) {
                                    fread(content, 1, size, file);
                                    content[size] = '\0';
                                }
                                fclose(file);
                            }
                            success = unlink(files[selected]->full_path) == 0;
                        } else if (S_ISDIR(files[selected]->mode)) {
                            // Сохраняем содержимое директории
                            save_directory_contents(files[selected]->full_path, dir_contents, &dir_content_count);
                            success = remove_directory(files[selected]->full_path, dir_contents, &dir_content_count) == 0;
                        } else {
                            success = unlink(files[selected]->full_path) == 0;
                        }

                        if (success) {
                            mvprintw(max_y - 2, 1, S_ISDIR(files[selected]->mode) ? "Directory deleted" : S_ISLNK(files[selected]->mode) ? "Link deleted" : "File deleted");
                            // Добавляем в стек undo
                            if (undo_count < MAX_UNDO) {
                                undo_stack[undo_count].type = ACTION_DELETE;
                                undo_stack[undo_count].path = strdup(files[selected]->full_path);
                                undo_stack[undo_count].content = content;
                                undo_stack[undo_count].dir_contents = dir_content_count > 0 ? malloc(dir_content_count * sizeof(DirContent)) : NULL;
                                if (undo_stack[undo_count].dir_contents) {
                                    memcpy(undo_stack[undo_count].dir_contents, dir_contents, dir_content_count * sizeof(DirContent));
                                }
                                undo_stack[undo_count].dir_content_count = dir_content_count;
                                undo_count++;
                            } else {
                                free(content);
                                for (int i = 0; i < dir_content_count; i++) {
                                    free(dir_contents[i]->path);
                                    if (dir_contents[i]->content) free(dir_contents[i]->content);
                                    free(dir_contents[i]);
                                }
                            }
                            // Удаляем из списка
                            free(files[selected]->full_path);
                            free(files[selected]->display_path);
                            free(files[selected]);
                            for (int i = selected; i < count - 1; i++) {
                                files[i] = files[i + 1];
                            }
                            count--;
                            if (selected >= count && count > 0) selected--;
                        } else {
                            mvprintw(max_y - 2, 1, "Delete failed");
                            free(content);
                            for (int i = 0; i < dir_content_count; i++) {
                                free(dir_contents[i]->path);
                                if (dir_contents[i]->content) free(dir_contents[i]->content);
                                free(dir_contents[i]);
                            }
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'm':
                if (selected < count) {
                    if (confirm_dialog(dialog_win, "Change permissions?")) {
                        if (change_permissions(files[selected]->full_path, dialog_win) == 0) {
                            mvprintw(max_y - 2, 1, "Permissions changed");
                            struct stat stat_block;
                            if (lstat(files[selected]->full_path, &stat_block) != -1) {
                                files[selected]->mode = stat_block.st_mode;
                            }
                        } else {
                            mvprintw(max_y - 2, 1, "Failed to change permissions");
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'n':
                if (confirm_dialog(dialog_win, "Create new file/dir/link?")) {
                    if (create_object(dir_path, dialog_win) == 0) {
                        mvprintw(max_y - 2, 1, "Object created");
                        for (int i = 0; i < count; i++) {
                            free(files[i]->full_path);
                            free(files[i]->display_path);
                            free(files[i]);
                        }
                        count = 0;
                        dirwalk(dir_path, files, &count, dir_path);
                        qsort(files, count, sizeof(FileInfo *), compare_files);
                        selected = 0;
                        offset = 0;
                    } else {
                        mvprintw(max_y - 2, 1, "Failed to create object");
                    }
                    clrtoeol();
                    refresh();
                }
                break;
            case 'e':
                if (selected < count) {
                    if (!S_ISREG(files[selected]->mode)) {
                        wclear(dialog_win);
                        box(dialog_win, 0, 0);
                        mvwprintw(dialog_win, 1, 1, "Error: Can only edit regular files");
                        wrefresh(dialog_win);
                        getch();
                        wclear(dialog_win);
                        wrefresh(dialog_win);
                    } else if (confirm_dialog(dialog_win, "Edit file?")) {
                        if (edit_file(files[selected]->full_path, dialog_win) == 0) {
                            mvprintw(max_y - 2, 1, "File edited");
                            struct stat stat_block;
                            if (lstat(files[selected]->full_path, &stat_block) != -1) {
                                files[selected]->size = stat_block.st_size;
                                files[selected]->mtime = stat_block.st_mtime;
                            }
                        } else {
                            mvprintw(max_y - 2, 1, "Failed to edit file");
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'r':
                if (selected < count) {
                    if (confirm_dialog(dialog_win, "Rename file?")) {
                        if (rename_file(files[selected]->full_path, dialog_win, dir_path) == 0) {
                            mvprintw(max_y - 2, 1, "File renamed");
                            for (int i = 0; i < count; i++) {
                                free(files[i]->full_path);
                                free(files[i]->display_path);
                                free(files[i]);
                            }
                            count = 0;
                            dirwalk(dir_path, files, &count, dir_path);
                            qsort(files, count, sizeof(FileInfo *), compare_files);
                            selected = 0;
                            offset = 0;
                        } else {
                            mvprintw(max_y - 2, 1, "Failed to rename file");
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'p':
                if (selected < count) {
                    if (confirm_dialog(dialog_win, "Move file?")) {
                        if (move_file(files[selected]->full_path, dialog_win) == 0) {
                            mvprintw(max_y - 2, 1, "File moved");
                            for (int i = 0; i < count; i++) {
                                free(files[i]->full_path);
                                free(files[i]->display_path);
                                free(files[i]);
                            }
                            count = 0;
                            dirwalk(dir_path, files, &count, dir_path);
                            qsort(files, count, sizeof(FileInfo *), compare_files);
                            selected = 0;
                            offset = 0;
                        } else {
                            mvprintw(max_y - 2, 1, "Failed to move file");
                        }
                        clrtoeol();
                        refresh();
                    }
                }
                break;
            case 'u':
                if (confirm_dialog(dialog_win, "Undo last action?")) {
                    if (undo_last_action(files, &count, dir_path) == 0) {
                        mvprintw(max_y - 2, 1, "Action undone");
                        selected = 0;
                        offset = 0;
                    } else {
                        mvprintw(max_y - 2, 1, "Nothing to undo");
                    }
                    clrtoeol();
                    refresh();
                }
                break;
            case 'v':
                if (selected < count && S_ISREG(files[selected]->mode)) {
                    if (confirm_dialog(dialog_win, "View file?")) {
                        if (view_file(files[selected]->full_path, view_win) == 0) {
                            mvprintw(max_y - 2, 1, "File viewed");
                        } else {
                            mvprintw(max_y - 2, 1, "Failed to view file");
                        }
                        wclear(view_win);
                        wrefresh(view_win);
                        clrtoeol();
                        refresh();
                    }
                } else {
                    wclear(dialog_win);
                    box(dialog_win, 0, 0);
                    mvwprintw(dialog_win, 1, 1, "Error: Can only view regular files");
                    wrefresh(dialog_win);
                    getch();
                    wclear(dialog_win);
                    wrefresh(dialog_win);
                }
                break;
            default:
                continue;
        }
        display_files(file_win, files, count, selected, offset);
        display_info(info_win, selected < count ? files[selected] : NULL);
    }

    // Очистка
    for (int i = 0; i < count; i++) {
        free(files[i]->full_path);
        free(files[i]->display_path);
        free(files[i]);
    }
    for (int i = 0; i < undo_count; i++) {
        free(undo_stack[i].path);
        if (undo_stack[i].old_path) free(undo_stack[i].old_path);
        if (undo_stack[i].content) free(undo_stack[i].content);
        if (undo_stack[i].dir_contents) {
            for (int j = 0; j < undo_stack[i].dir_content_count; j++) {
                free(undo_stack[i].dir_contents[j].path);
                if (undo_stack[i].dir_contents[j].content) free(undo_stack[i].dir_contents[j].content);
            }
            free(undo_stack[i].dir_contents);
        }
    }
    delwin(file_win);
    delwin(info_win);
    delwin(dialog_win);
    delwin(view_win);
    endwin();
    return 0;
}