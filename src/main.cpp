#include <cstring>
#include <cstdlib>
#include <climits>

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gsl-lite.h"
#include "entry.hpp"

#ifdef USE_GIT
    #include <git2.h>
#endif

#include <iniparser.h>

using FileList = std::vector<Entry *>;
using DirList = std::map<std::string, FileList>;

settings_t settings = {0};

void initcolors()
{
    const char *ls_colors = std::getenv("LS_COLORS");

    std::stringstream ss;
    ss << ls_colors;

    std::string token;
    Color color;

    while (std::getline(ss, token, ':')) {
        size_t pos = token.find('=');

        color.glob = token.substr(0, pos);
        color.color = token.substr(pos + 1);
        colors.push_back(color);
    }

    /* for (auto c : colors) { */
    /*     printf("\033[%sm%s\033[0m\n", c.color.c_str(), c.glob.c_str()); */
    /* } */
}

#ifdef USE_GIT
unsigned int dirflags(git_repository *repo, std::string rp, std::string path)
{
    unsigned int flags = GIT_DIR_CLEAN;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = (
                     GIT_STATUS_OPT_INCLUDE_UNMODIFIED |
                     GIT_STATUS_OPT_EXCLUDE_SUBMODULES
                 );

    if (repo == nullptr) {
        opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

        git_buf root = {0};

        int error = git_repository_discover(&root, path.c_str(), 0, NULL);

        if (error >= 0) {
            error = git_repository_open(&repo, root.ptr);

            if (error < 0) {
                fprintf(stderr, "Unable to open git repository at %s", root.ptr);
                git_buf_free(&root);
                return UINT_MAX;
            }

            rp = root.ptr;
            rp.replace(rp.end() - 5, rp.end(), ""); // Remove .git
        } else {
            git_buf_free(&root);
            return UINT_MAX;
        }

        flags |= GIT_ISREPO;

        path.replace(path.begin(), path.begin() + rp.length(), "");
        git_buf_free(&root);
    }

    opts.pathspec.count = 1;
    opts.pathspec.strings = new char *[1];
    opts.pathspec.strings[0] = const_cast<char *>(path.c_str());

    git_status_list *statuses = NULL;
    git_status_list_new(&statuses, repo, &opts);

    size_t count = git_status_list_entrycount(statuses);

    for (size_t i = 0; i < count; ++i) {
        const git_status_entry *entry = git_status_byindex(statuses, i);

        if (entry->status != 0) {
            flags |= GIT_DIR_DIRTY;
            break;
        }
    }

    git_repository_free(repo);
    return flags;
}
#endif

Entry *addfile(const char *path, const char *file)
{
    struct stat st = {0};
    std::string directory = path;

    if (!directory.empty()) {
        directory += '/';
    }

    #ifdef USE_GIT
    std::string rp;
    git_buf root = {0};
    git_repository *repo = nullptr;

    int error = git_repository_discover(&root, directory.c_str(), 0, NULL);

    if (error >= 0) {
        error = git_repository_open(&repo, root.ptr);

        if (error < 0) {
            fprintf(stderr, "Unable to open git repository at %s", root.ptr);
            return nullptr;
        }

        rp = root.ptr;
        rp.replace(rp.end() - 5, rp.end(), ""); // Remove .git
    }

    #endif

    char fullpath[PATH_MAX] = {0};
    snprintf(&fullpath[0], PATH_MAX, "%s%s", directory.c_str(), file);

    if ((lstat(&fullpath[0], &st)) < 0) {
        fprintf(stderr, "Unable to get stats for %s\n", &fullpath[0]);

        #ifdef USE_GIT
        git_repository_free(repo);
        git_buf_free(&root);
        #endif

        return nullptr;
    }

    #ifdef S_ISLNK

    if (S_ISLNK(st.st_mode) && settings.resolve_links) {
        char target[PATH_MAX] = {0};

        if ((readlink(&fullpath[0], &target[0], sizeof(target))) >= 0) {
            std::string lpath = &target[0];

            char linkpath[PATH_MAX] = {0};

            if (target[0] != '/') {
                lpath = directory + "/" + std::string(&target[0]);
            }

            if (realpath(lpath.c_str(), &linkpath[0]) == nullptr) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                return new Entry(
                           directory,
                           file,
                           &fullpath[0],
                           nullptr
                       );
            }

            if ((lstat(&linkpath[0], &st)) < 0) {
                fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                );

                return new Entry(
                           directory,
                           file,
                           &fullpath[0],
                           nullptr
                       );
            }

            strncpy(&fullpath[0], &linkpath[0], PATH_MAX);
        }
    }

    #endif /* S_ISLNK */

    unsigned int flags = UINT_MAX;

    #ifdef USE_GIT

    if (repo != nullptr) {
        flags = 0;
        char dirpath[PATH_MAX] = {0};
        if(!realpath((directory + file).c_str(), &dirpath[0])) {
            fprintf(
                    stderr,
                    "cannot access '%s': No such file or directory\n",
                    file
                   );

            return new Entry(
                    directory,
                    file,
                    &fullpath[0],
                    nullptr
                );
        }

        std::string path = dirpath;
        path.replace(path.begin(), path.begin() + rp.length(), "");

        if (path.length()) {
            while (path.at(0) == '/') {
                path.replace(path.begin(), path.begin() + 1, "");
            }
        }

        if (S_ISDIR(st.st_mode)) {
            flags = dirflags(repo, rp, path);
        } else {
            git_status_file(&flags, repo, path.c_str());
        }
    } else {
        if (S_ISDIR(st.st_mode)) {
            flags = dirflags(nullptr, "", directory + file);
        }
    }

    #endif

    return new Entry(
               directory,
               file,
               &fullpath[0],
               &st,
               flags
           );

    #ifdef USE_GIT
    git_repository_free(repo);
    git_buf_free(&root);
    #endif
}

FileList listdir(const char *path)
{
    FileList lst;
    DIR *dir;

    if ((dir = opendir(path)) != nullptr) {
        dirent *ent;

        while ((ent = readdir((dir))) != nullptr) {
            if (
                strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0
            ) {
                continue;
            }

            if (ent->d_name[0] == '.' && !settings.show_hidden) {
                continue;
            }

            auto f = addfile(path, &ent->d_name[0]);

            if (f != nullptr) {
                lst.push_back(f);
            }
        }

        closedir(dir);
    }

    return lst;
}

void printdir(FileList *lst)
{
    std::sort(lst->begin(), lst->end(), [](Entry * a, Entry * b) {
        if (settings.dirs_first) {
            if (a->isdir && !b->isdir) {
                return true;
            }

            if (!a->isdir && b->isdir) {
                return false;
            }
        }

        int64_t cmp = 0;

        switch (settings.sort) {
            case SORT_ALPHA:
                cmp = b->file.compare(a->file);
                break;

            case SORT_MODIFIED:
                cmp = a->modified - b->modified;
                break;

            case SORT_SIZE:
                cmp = a->bsize - b->bsize;
                break;
        }

        if (settings.reversed) {
            return cmp < 0;
        } else {
            return cmp > 0;
        }
    });

    if (settings.list) {
        size_t max_user = 0;
        size_t max_date = 0;
        size_t max_date_unit = 0;
        size_t max_size = 0;

        for (const auto l : *lst) {
            max_user = std::max(l->user_len, max_user);
            max_date = std::max(l->date_len,  max_date);
            max_date_unit = std::max(l->date_unit_len, max_date_unit);
            max_size = std::max(l->size_len, max_size);
        }

        for (const auto l : *lst) {
            l->list(max_user, max_date, max_date_unit, max_size);
        }
    } else {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

        size_t max_len = 0;

        for (const auto l : *lst) {
            max_len = std::max(l->file_len, max_len);
        }

        /* max_len += 10; // for icons, colors etc. */
        int calc = ((float)w.ws_col / (float)max_len) - 2;
        int columns = std::max(calc, 1);

        int current = 0;

        for (const auto l : *lst) {
            l->print(max_len);
            current++;

            if (current == columns)  {
                printf("\n");
                current = 0;
            }
        }

        printf("\n");
    }
}

const char *gethome()
{
    const char *homedir = getenv("HOME");

    if (homedir != 0) {
        return homedir;
    }

    struct passwd *result = getpwuid(getuid());

    if (result == 0) {
        fprintf(stderr, "Unable to find home-directory\n");
        exit(EXIT_FAILURE);
    }

    homedir = result->pw_dir;

    return homedir;
}

bool exists(const char *name)
{
    struct stat buffer;
    return (stat(name, &buffer) == 0);
}

const char *cpp11_getstring(dictionary *d, const char *key,
                            const char *def)
{
    return iniparser_getstring(d, key, const_cast<char *>(def));
}

void loadconfig()
{
    dictionary *ini = 0;
    char filename[PATH_MAX] = {0};
    char file[255] = {"/lsext.ini"};

    const char *confdir = getenv("XDG_CONFIG_HOME");

    if (confdir == 0) {
        sprintf(file, "/.lsext.ini");
        confdir = gethome();
    } else {
        sprintf(filename, "%s%s", confdir, file);

        if (!exists(filename)) {
            sprintf(file, "/.lsext.ini");
            confdir = gethome();
        }
    }

    sprintf(filename, "%s%s", confdir, file);

    if (!exists(filename)) {
        sprintf(filename, "./lsext.ini"); // Useful when debugging
    }

    if (exists(filename)) {
        ini = iniparser_load(filename);
    }

    settings.size_number_color = iniparser_getboolean(ini, "settings:size_number_color", true);
    settings.date_number_color = iniparser_getboolean(ini, "settings:date_number_color", true);

    settings.show_hidden = iniparser_getboolean(ini, "settings:show_hidden", false);
    settings.list = iniparser_getboolean(ini, "settings:list", false);
    settings.resolve_links = iniparser_getboolean(ini, "settings:resolve_links", false);
    settings.reversed = iniparser_getboolean(ini, "settings:reversed", false);
    settings.dirs_first = iniparser_getboolean(ini, "settings:dirs_first", true);
    settings.sort = static_cast<sort_t>(
        iniparser_getint(ini, "settings:sort", SORT_ALPHA)
    );

    settings.colors = iniparser_getboolean(ini, "settings:colors", true);

    settings.color.perm.none = iniparser_getint(ini, "colors:perm_none", 0);
    settings.color.perm.exec = iniparser_getint(ini, "colors:perm_exec", 2);
    settings.color.perm.read = iniparser_getint(ini, "colors:perm_read", 3);
    settings.color.perm.write = iniparser_getint(ini, "colors:perm_write", 1);

    settings.color.perm.dir = iniparser_getint(ini, "colors:perm_dir", 4);
    settings.color.perm.link = iniparser_getint(ini, "colors:perm_link", 6);
    settings.color.perm.sticky = iniparser_getint(ini, "colors:perm_sticky", 5);
    settings.color.perm.special = iniparser_getint(ini, "colors:perm_special", 5);
    settings.color.perm.block = iniparser_getint(ini, "colors:perm_block", 5);
    settings.color.perm.unknown = iniparser_getint(ini, "colors:perm_unknown", 1);
    settings.color.perm.other = iniparser_getint(ini, "colors:perm_other", 7);

    settings.color.user.user = iniparser_getint(ini, "colors:user", 11);
    settings.color.user.group = iniparser_getint(ini, "colors:group", 3);
    settings.color.user.separator = iniparser_getint(ini, "colors:user_separator", 0);

    settings.color.size.number = iniparser_getint(ini, "colors:size_number", 12);

    settings.color.size.byte = iniparser_getint(ini, "colors:size_byte", 4);
    settings.color.size.kilo = iniparser_getint(ini, "colors:size_kilo", 4);
    settings.color.size.mega = iniparser_getint(ini, "colors:size_mega", 4);
    settings.color.size.giga = iniparser_getint(ini, "colors:size_giga", 4);
    settings.color.size.tera = iniparser_getint(ini, "colors:size_tera", 4);
    settings.color.size.peta = iniparser_getint(ini, "colors:size_peta", 4);

    settings.color.date.number = iniparser_getint(ini, "colors:date_number", 10);

    settings.color.date.sec = iniparser_getint(ini, "colors:date_sec", 2);
    settings.color.date.min = iniparser_getint(ini, "colors:date_min", 2);
    settings.color.date.hour = iniparser_getint(ini, "colors:date_hour", 2);
    settings.color.date.day = iniparser_getint(ini, "colors:date_day", 2);
    settings.color.date.mon = iniparser_getint(ini, "colors:date_mon", 2);
    settings.color.date.year = iniparser_getint(ini, "colors:date_year", 2);
    settings.color.date.other = iniparser_getint(ini, "colors:date_other", 2);

    settings.symbols.user.separator = cpp11_getstring(ini, "symbols:user_separator", ":");

    settings.symbols.size.byte = cpp11_getstring(ini, "symbols:size_byte", "B");
    settings.symbols.size.kilo = cpp11_getstring(ini, "symbols:size_kilo", "K");
    settings.symbols.size.mega = cpp11_getstring(ini, "symbols:size_mega", "M");
    settings.symbols.size.giga = cpp11_getstring(ini, "symbols:size_giga", "G");
    settings.symbols.size.tera = cpp11_getstring(ini, "symbols:size_tera", "T");
    settings.symbols.size.peta = cpp11_getstring(ini, "symbols:size_peta", "P");

    settings.symbols.date.sec = cpp11_getstring(ini, "symbols:date_sec", "sec");
    settings.symbols.date.min = cpp11_getstring(ini, "symbols:date_min", "min");
    settings.symbols.date.hour = cpp11_getstring(ini, "symbols:date_hour", "hour");
    settings.symbols.date.day = cpp11_getstring(ini, "symbols:date_day", "day");
    settings.symbols.date.mon = cpp11_getstring(ini, "symbols:date_mon", "mon");
    settings.symbols.date.year = cpp11_getstring(ini, "symbols:date_year", "year");

    #ifdef USE_GIT
    settings.symbols.git.ignore = cpp11_getstring(ini, "symbols:git_ignore", "!");
    settings.symbols.git.conflict = cpp11_getstring(ini, "symbols:git_conflict", "X");
    settings.symbols.git.modified = cpp11_getstring(ini, "symbols:git_modified", "~");
    settings.symbols.git.renamed = cpp11_getstring(ini, "symbols:git_renamed", "R");
    settings.symbols.git.added= cpp11_getstring(ini, "symbols:git_added", "+");
    settings.symbols.git.typechange = cpp11_getstring(ini, "symbols:git_typechange", "T");
    settings.symbols.git.unreadable = cpp11_getstring(ini, "symbols:git_unreadable", "-");
    settings.symbols.git.untracked = cpp11_getstring(ini, "symbols:git_untracked", "?");
    settings.symbols.git.unchanged = cpp11_getstring(ini, "symbols:git_unchanged", " ");

    settings.symbols.git.dir_dirty = cpp11_getstring(ini, "symbols:git_dir_dirty", "!");
    settings.symbols.git.dir_clean = cpp11_getstring(ini, "symbols:git_dir_clean", " ");
    settings.symbols.git.repo_dirty = cpp11_getstring(ini, "symbols:git_repo_dirty", "!");
    settings.symbols.git.repo_clean = cpp11_getstring(ini, "symbols:git_repo_clean", "@");

    settings.color.git.ignore = iniparser_getint(ini, "colors:git_ignore", 0);
    settings.color.git.conflict = iniparser_getint(ini, "colors:git_conflict", 1);
    settings.color.git.modified = iniparser_getint(ini, "colors:git_modified", 3);
    settings.color.git.renamed = iniparser_getint(ini, "colors:git_renamed", 5);
    settings.color.git.added = iniparser_getint(ini, "colors:git_added", 2);
    settings.color.git.typechange = iniparser_getint(ini, "colors:git_typechange", 4);
    settings.color.git.unreadable = iniparser_getint(ini, "colors:git_unreadable", 9);
    settings.color.git.untracked = iniparser_getint(ini, "colors:git_untracked", 8);
    settings.color.git.unchanged = iniparser_getint(ini, "colors:git_unchanged", 0);

    settings.color.git.dir_dirty = iniparser_getint(ini, "colors:git_dir_dirty", 1);
    settings.color.git.dir_clean = iniparser_getint(ini, "colors:git_dir_clean", 0);
    settings.color.git.repo_dirty = iniparser_getint(ini, "colors:git_repo_dirty", 1);
    settings.color.git.repo_clean = iniparser_getint(ini, "colors:git_repo_clean", 2);
    #endif

    iniparser_freedict(ini);
}

int main(int argc, const char *argv[])
{
    FileList files;
    DirList dirs;

    loadconfig();

    bool parse = true;

    while (parse) {
        int c = getopt(argc, const_cast<char **>(argv), "AalrtfSLnh");

        switch (c) {
            case 'L':
                settings.resolve_links = !settings.resolve_links;
                break;

            case 'a':
                settings.show_hidden = !settings.show_hidden;
                break;

            case 'r':
                settings.reversed = !settings.reversed;
                break;

            case 'f':
                settings.dirs_first = !settings.dirs_first;
                break;

            case 't':
                settings.sort = SORT_MODIFIED;
                break;

            case 'S':
                settings.sort = SORT_SIZE;
                break;

            case 'A':
                settings.sort = SORT_ALPHA;
                break;

            case 'l':
                settings.list = !settings.list;
                break;

            case 'n':
                settings.colors = !settings.colors;
                break;

            case 'h':
                printf("\nTODO: Add help.\n\n");
                return EXIT_SUCCESS;

            default:
                parse = false;
                break;
        }
    }

    #ifdef USE_GIT
    git_libgit2_init();
    #endif

    if (settings.colors) {
        initcolors();
    }

    if (argc - optind > 0) {
        auto sp = gsl::make_span<const char *>(argv + optind, argv + argc);
        std::sort(sp.begin(), sp.end(), [](const char *a, const char *b) {
            return strlen(a) < strlen(b);
        });

        for (int i = 0; i < argc - optind; i++) {
            struct stat st = {0};

            if ((lstat(sp.at(i), &st)) < 0) {
                return EXIT_FAILURE;
            }

            if (S_ISDIR(st.st_mode)) {
                dirs.insert(DirList::value_type(sp.at(i), listdir(sp.at(i))));
            } else {
                char file[PATH_MAX] = {0};
                strncpy(&file[0], sp.at(i), PATH_MAX);

                files.push_back(addfile("", &file[0]));
            }
        }
    } else {
        dirs.insert(DirList::value_type("./", listdir(".")));
    }

    if (!files.empty()) {
        printdir(&files);
    }

    for (auto dir : dirs) {
        if (dirs.size() > 1 || !files.empty()) {
            fprintf(stdout, "\n%s:\n", dir.first.c_str());
        }

        printdir(&dir.second);
    }

    files.clear();
    dirs.clear();

    #ifdef USE_GIT
    git_libgit2_shutdown();
    #endif

    return EXIT_SUCCESS;
}