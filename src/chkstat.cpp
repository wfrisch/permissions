/* Copyright (c) 2004 SuSE Linux AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 ****************************************************************
 */

#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/param.h>
#include <limits.h>

// local headers
#include "chkstat.h"
#include "formatting.h"
#include "utility.h"

// C++
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#define _STRINGIFY(s) #s
#define STRINGIFY(s) _STRINGIFY(s)

#define PROC_FD_PATH_SIZE (sizeof("/proc/self/fd/") + sizeof(STRINGIFY(INT_MAX)))

int
Chkstat::safe_open(const char *path, struct stat *stb, uid_t target_uid, bool *traversed_insecure)
{
    char pathbuf[PATH_MAX];
    char *path_rest;
    int lcnt;
    int pathfd = -1;
    int parentfd = -1;
    struct stat root_st;
    bool is_final_path_element = false;
    const auto &altroot = m_root_path.getValue();

    *traversed_insecure = false;

    lcnt = 0;
    if ((size_t)snprintf(pathbuf, sizeof(pathbuf), "%s", path + altroot.length()) >= sizeof(pathbuf))
        goto fail;

    path_rest = pathbuf;

    while (!is_final_path_element)
    {
        *path_rest = '/';
        char *cursor = path_rest + 1;

        if (pathfd == -1)
        {
            const auto root = altroot.empty() ? std::string("/") : altroot;
            pathfd = open(root.c_str(), O_PATH | O_CLOEXEC);

            if (pathfd == -1)
            {
                fprintf(stderr, "failed to open root directory %s: %s\n", root.c_str(), strerror(errno));
                goto fail;
            }

            if (fstat(pathfd, &root_st))
            {
                fprintf(stderr, "failed to stat root directory %s: %s\n", root.c_str(), strerror(errno));
                goto fail;
            }
            // stb and pathfd must be in sync for the root-escape check below
            memcpy(stb, &root_st, sizeof(*stb));
        }

        path_rest = strchr(cursor, '/');
        // path_rest is NULL when we reach the final path element
        is_final_path_element = path_rest == NULL || strcmp("/", path_rest) == 0;

        if (!is_final_path_element)
        {
            *path_rest = 0;
        }

        // multiple consecutive slashes: ignore
        if (!is_final_path_element && *cursor == '\0')
            continue;

        // never move up from the configured root directory (using the stat result from the previous loop iteration)
        if (strcmp(cursor, "..") == 0 && !altroot.empty() && stb->st_dev == root_st.st_dev && stb->st_ino == root_st.st_ino)
            continue;

        // cursor is an empty string for trailing slashes, open again with different open_flags.
        int newpathfd = openat(pathfd, *cursor ? cursor : ".", O_PATH | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (newpathfd == -1)
            goto fail;

        close(pathfd);
        pathfd = newpathfd;

        if (fstat(pathfd, stb))
            goto fail;

        /* owner of directories must be trusted for setuid/setgid/capabilities as we have no way to verify file contents */
        /* for euid != 0 it is also ok if the owner is euid */
        if (stb->st_uid && stb->st_uid != m_euid && !is_final_path_element)
        {
            *traversed_insecure = true;
        }
        // path is in a world-writable directory, or file is world-writable itself.
        else if (!S_ISLNK(stb->st_mode) && (stb->st_mode & S_IWOTH) && !is_final_path_element)
        {
            *traversed_insecure = true;
        }

        // if parent directory is not owned by root, the file owner must match the owner of parent
        if (stb->st_uid && stb->st_uid != target_uid && stb->st_uid != m_euid)
        {
            if (is_final_path_element)
            {
                fprintf(stderr, "%s: has unexpected owner. refusing to correct due to unknown integrity.\n", path+altroot.length());
                goto fail;
            }
            else
                goto fail_insecure_path;
        }

        if (S_ISLNK(stb->st_mode))
        {
            // If the path configured in the permissions configuration is a symlink, we don't follow it.
            // This is to emulate legacy behaviour: old insecure versions of chkstat did a simple lstat(path) as 'protection' against malicious symlinks.
            if (is_final_path_element || ++lcnt >= 256)
                goto fail;

            // Don't follow symlinks owned by regular users.
            // In theory, we could also trust symlinks where the owner of the target matches the owner
            // of the link, but we're going the simple route for now.
            if (stb->st_uid && stb->st_uid != m_euid)
                goto fail_insecure_path;

            char linkbuf[PATH_MAX];
            ssize_t l = readlinkat(pathfd, "", linkbuf, sizeof(linkbuf) - 1);

            if (l <= 0 || (size_t)l >= sizeof(linkbuf) - 1)
                goto fail;

            while(l && linkbuf[l - 1] == '/')
            {
                l--;
            }

            linkbuf[l] = 0;

            if (linkbuf[0] == '/')
            {
                // absolute link
                close(pathfd);
                pathfd = -1;
            }
            else
            {
                // relative link: continue relative to the parent directory
                close(pathfd);
                if (parentfd == -1) // we encountered a link directly below /
                    pathfd = -1;
                else
                    pathfd = dup(parentfd);
            }

            // need a temporary buffer because path_rest points into pathbuf
            // and snprintf doesn't allow the same buffer as source and
            // destination
            char tmp[sizeof(pathbuf) - 1];
            size_t len = (size_t)snprintf(tmp, sizeof(tmp), "%s/%s", linkbuf, path_rest + 1);
            if (len >= sizeof(tmp))
                goto fail;

            // the first byte of path_rest is always set to a slash at the start of the loop, so we offset by one byte
            strcpy(pathbuf + 1, tmp);
            path_rest = pathbuf;
        }
        else if (S_ISDIR(stb->st_mode))
        {
            if (parentfd >= 0)
                close(parentfd);
            // parentfd is only needed to find the parent of a symlink.
            // We can't encounter links when resolving '.' or '..' so those don't need any special handling.
            parentfd = dup(pathfd);
        }
    }

    // world-writable file: error out due to unknown file integrity
    if (S_ISREG(stb->st_mode) && (stb->st_mode & S_IWOTH))
    {
        fprintf(stderr, "%s: file has insecure permissions (world-writable)\n", path+altroot.length());
        goto fail;
    }

    if (parentfd >= 0)
    {
        close(parentfd);
    }

    return pathfd;
fail_insecure_path:

    {
        char linkpath[PATH_MAX] = "ancestor";
        char procpath[PROC_FD_PATH_SIZE];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", pathfd);
        ssize_t l = readlink(procpath, linkpath, sizeof(linkpath) - 1);
        if (l > 0)
        {
            linkpath[MIN((size_t)l, sizeof(linkpath) - 1)] = '\0';
        }
        fprintf(stderr, "%s: on an insecure path - %s has different non-root owner who could tamper with the file.\n", path+altroot.length(), linkpath);
    }

fail:
    if (pathfd >= 0)
    {
        close(pathfd);
    }
    if (parentfd >= 0)
    {
        close(parentfd);
    }
    return -1;
}


Chkstat::Chkstat(int argc, const char **argv) :
    m_argc(argc),
    m_argv(argv),
    m_parser("Tool to check and set file permissions"),
    m_system_mode("", "system", "system mode, act according to /etc/sysconfig/security", m_parser),
    m_force_fscaps("", "fscaps", "force use of file system capabilities", m_parser),
    m_disable_fscaps("", "no-fscaps", "disable use of file system capabilities", m_parser),
    m_apply_changes("s", "set", "actually apply changes (--system mode may imply this depending on file based configuration)", m_parser),
    m_only_warn("", "warn", "only inform about which changes would be performed but don't actually apply them (which is the default, except in --system mode)", m_parser),
    m_no_header("n", "noheader", "don't print intro message", m_parser),
    m_examine_paths("e", "examine", "operate only on the specified path(s)", false, "PATH", m_parser),
    m_force_level_list("", "level", "force application of the specified space-separated list of security levels (only supported in --system mode)", false, "", "e.g. \"local paranoid\"", m_parser),
    m_file_lists("f", "files", "read newline separated list of files to check (see --examine) from the specified path", false, "PATH", m_parser),
    m_root_path("r", "root", "check files relative to the given root directory", false, "", "PATH", m_parser),
    m_config_root_path("", "config-root", "lookup configuration files relative to the given root directory", false, "", "PATH", m_parser),
    m_input_args("args", "in --system mode a list of paths to check, otherwise a list of profiles to parse", false, "PATH", m_parser),
    m_euid(geteuid())
{
}

bool Chkstat::validateArguments()
{
    // exits on error/usage
    m_parser.parse(m_argc, m_argv);

    bool ret = true;

    const auto xor_args = {
        std::make_pair(&m_system_mode, static_cast<TCLAP::SwitchArg*>(&m_apply_changes)),
        {&m_force_fscaps, &m_disable_fscaps},
        {&m_apply_changes, &m_only_warn}
    };

    for (const auto &args: xor_args)
    {
        const auto &arg1 = *args.first;
        const auto &arg2 = *args.second;

        if (arg1.isSet() && arg2.isSet())
        {
            std::cerr << arg1.getName() << " and " << arg2.getName()
                << " cannot be set at the same time\n";
            ret = false;
        }
    }

    if (!m_system_mode.isSet() && m_input_args.getValue().empty())
    {
        std::cerr << "one or more permission file paths to use are required\n";
        ret = false;
    }

    for (const auto arg: { &m_root_path, &m_config_root_path })
    {
        if (!arg->isSet())
            continue;

        auto &path = arg->getValue();

        if (path.empty() || path[0] != '/')
        {
            std::cerr << arg->getName() << " must begin with '/'\n";
            ret = false;
        }

        while (*path.rbegin() == '/')
        {
            // remove trailing slashes to normalize arguments
            path.pop_back();
        }
    }

    if (m_config_root_path.isSet())
    {
        const auto &path = m_config_root_path.getValue();

        // considering NAME_MAX characters left is somewhat arbitrary, but
        // staying within these limits should at least allow us to not
        // encounter ENAMETOOLONG in typical setups
        if (path.length() >= (PATH_MAX - NAME_MAX -1))
        {
            std::cerr << m_config_root_path.getName() << ": prefix is too long\n";
            ret = false;
        }
    }

    return ret;
}

bool Chkstat::processArguments()
{
    for (const auto &path: m_examine_paths.getValue())
    {
        m_checklist.insert(path);
    }

    for (const auto &path: m_file_lists.getValue())
    {
        std::ifstream fs(path);

        if( !fs )
        {
            std::cerr << m_file_lists.getName() << ": " << path << ": " << strerror(errno) << std::endl;
            return false;
        }

        std::string line;

        while (std::getline(fs, line))
        {
            if (line.empty())
                continue;
            m_checklist.insert(line);
        }
    }

    return true;
}

static inline std::string& stripQuotes(std::string &s)
{
    return strip(s, [](char c) { return c == '"' || c == '\''; });
}

bool Chkstat::parseSysconfig()
{
    const auto file = m_config_root_path.getValue() + "/etc/sysconfig/security";
    std::ifstream fs(file);

    if (!fs)
    {
        std::cerr << "error opening " << file << ": " << strerror(errno) << std::endl;
        return false;
    }

    std::string line;
    std::string key;
    std::string value;
    size_t linenr = 0;
    bool check_permissions = true;

    while (std::getline(fs, line))
    {
        linenr++;
        strip(line);

        if (line.empty() || line[0] == '#')
            continue;

        const auto sep = line.find_first_of('=');
        if (sep == line.npos)
        {
            // syntax error?
            std::cerr << file << ":" << linenr << ": parse error in '" << line << "'" << std::endl;
            continue;
        }

        key = line.substr(0, sep);
        value = line.substr(sep + 1);
        value = stripQuotes(value);

        if (key == "PERMISSION_SECURITY")
        {
            if (m_force_level_list.isSet())
                // explicit levels are specified on the command line
                continue;

            // parse the space separated, ordered list of profiles to apply
            std::istringstream ss(value);
            std::string profile;

            while (std::getline(ss, profile, ' '))
            {
                if( profile != "local" && !profile.empty() )
                {
                    addProfile(profile);
                }
            }
        }
        // this setting was last part of the config file template in SLE-11
        // but we still support it in the code. The logic behind it is quite
        // complex since it is crossed with the command line settings
        else if (key == "CHECK_PERMISSIONS")
        {
            if (value == "set")
            {
                check_permissions = true;
            }
            else if (value == "no" || value.empty())
            {
                check_permissions = false;
            }
            else if (value != "warn")
            {
                std::cerr << file << ":" << linenr << ": invalid value for " << key << " (expected 'set', 'no' or 'warn'). Falling back to default value." << std::endl;
            }
        }
        else if (key == "PERMISSION_FSCAPS")
        {
            if (value == "yes")
            {
                m_use_fscaps = true;
            }
            else if (value == "no")
            {
                m_use_fscaps = false;
            }
        }
    }

    // apply the complex CHECK_PERMISSONS logic
    if (!m_apply_changes.isSet() && !m_only_warn.isSet())
    {
        if (check_permissions)
        {
            m_apply_changes.setValue(true);
        }
        else
        {
            std::cerr << "permissions handling disabled in " << file << std::endl;
            exit(0);
        }
    }

    return true;
}

bool Chkstat::checkFsCapsSupport() const
{
    /* check kernel capability support /sys/kernel/fscaps, 2.6.39 */
    std::ifstream fs("/sys/kernel/fscaps");

    if (!fs)
    {
        // if the file doesn't exist then there's probably no support for it
        return false;
    }

    size_t val = 0;
    fs >> val;

    if (fs.fail())
    {
        return false;
    }

    return val == 1;
}

void Chkstat::addProfile(const std::string &name)
{
    for (const auto &profile: m_profiles)
    {
        if (profile == name)
            // already exists
            return;
    }

    m_profiles.push_back(name);
}

void Chkstat::collectProfiles()
{
    /*
     * Since configuration files are in the process of separated between stock
     * configuration files in /usr and editable configuration files in /etc we
     * employ a backward compatibility logic here that prefers files in /usr
     * but also recognized files in /etc.
     */
    const auto &config_root = m_config_root_path.getValue();

    const auto usr_root = config_root + "/usr/share/permissions";
    const auto etc_root = config_root + "/etc";

    // 1. central fixed permissions file
    for (const auto &dir: {usr_root, etc_root})
    {
        const auto common_profile = dir + "/permissions";
        if (existsFile(common_profile))
        {
            m_profile_paths.push_back(common_profile);
            // only use the first one found
            break;
        }
    }

    // 2. central easy, secure paranoid as those are defined by SUSE
    for (const auto &profile: m_profiles)
    {
        if (!matchesAny(profile, PREDEFINED_PROFILES))
            continue;

        const auto base = std::string("/permissions.") + profile;

        for (const auto &dir: {usr_root, etc_root})
        {
            std::string profile_path = dir + base;

            if (existsFile(profile_path))
            {
                m_profile_paths.push_back(profile_path);
                // only use the first one found
                break;
            }
        }
    }

    // 3. package specific permissions
    // these files are owned by the individual packages
    // therefore while files are being moved from /etc to /usr we need to
    // consider both locations, not only the first matching one like above.
    for (const auto &dir: {usr_root, etc_root})
    {
        collectPackageProfiles(dir + "/permissions.d");
    }

    // 4. central permissions files with user defined level incl. 'local'
    for (const auto &profile: m_profiles)
    {
        if (matchesAny(profile, PREDEFINED_PROFILES))
            continue;

        const auto profile_path = etc_root + "/permissions." + profile;

        if (existsFile(profile_path))
        {
            m_profile_paths.push_back(profile_path);
        }
    }
}

void Chkstat::collectPackageProfiles(const std::string &dir)
{
    auto dir_handle = opendir(dir.c_str());

    if (!dir_handle)
    {
        // anything interesting here? probably ENOENT.
        return;
    }

    struct dirent* entry = nullptr;
    // first collect a sorted set of base files, skipping unwanted files like
    // backup files or specific profile files. Therefore filter out duplicates
    // using the sorted set.
    std::set<std::string> files;

    // TODO: error situations in readdir() are currently not recognized.
    // Consequences?
    while ((entry = readdir(dir_handle)))
    {
        std::string_view name(entry->d_name);

        if (name == "." || name == "..")
            continue;

        bool found_suffix = false;

        /* filter out backup files */
        for (const auto &suffix: {"~", ".rpmnew", ".rpmsave"})
        {
            if (hasSuffix(name, suffix))
            {
                found_suffix = true;
                break;
            }
        }

        if (found_suffix)
            continue;

        files.insert(std::string(name));
    }

    (void)closedir(dir_handle);

    // now add the sorted set of files to the profile paths to process later
    // on
    for (const auto &file: files)
    {
        const auto seppos = file.find_first_of('.');

        if (seppos != file.npos)
            // we're only interested in base profiles
            continue;

        const auto path = dir + "/" + file;

        m_profile_paths.push_back(path);

        /*
         * this is a bit of strange logic here, because we need to add the per
         * package profile files in the order as the profiles appear in
         * m_profiles.
         */
        for (const auto &profile: m_profiles)
        {
            const auto profile_basename = file + "." + profile;

            if (files.find(profile_basename) != files.end())
            {
                m_profile_paths.push_back(path + "." + profile);
            }
        }
    }
}

ProfileEntry&
Chkstat::addProfileEntry(const std::string &file, const std::string &owner, const std::string &group, mode_t mode)
{
    std::string path = file;

    if (!m_root_path.getValue().empty())
    {
        path = m_root_path.getValue() + '/' + file;
    }

    // this overwrites an possibly already existing entry
    // this is intended behaviour, hence the order in which profiles are
    // applied is important
    auto &entry = m_profile_entries[path];

    entry.file = path;
    entry.owner = owner;
    entry.group = group;
    entry.mode = mode;
    entry.caps.destroy();

    return entry;
}

static inline void badProfileLine(const std::string &file, const size_t line, const std::string &context)
{
    std::cerr << file << ":" << line << ": syntax error in permissions profile (" << context << ")" << std::endl;
}

bool Chkstat::parseExtraProfileLine(const std::string &line, ProfileEntry *entry)
{
        if (!entry)
        {
            return false;
        }
        else if (hasPrefix(line, "+capabilities "))
        {
            if (!m_use_fscaps)
                // ignore the content
                return true;

            auto cap_text = line.substr(line.find_first_of(' '));
            if (cap_text.empty())
                // ignore empty capability specification
                return true;

            entry->caps.setFromText(cap_text);
            return entry->caps.valid();
        }

        return false;
}

void Chkstat::parseProfile(const std::string &path)
{
    std::ifstream fs(path);

    if (!fs)
    {
        std::cerr << path << ": " << strerror(errno) << std::endl;
        exit(1);
    }

    size_t linenr = 0;
    ProfileEntry *last_entry = nullptr;
    std::string line;
    std::vector<std::string> parts;
    mode_t mode_int;

    // we're parsing lines of the following format here:
    //
    // # comment
    // <path> <user>:<group> <mode>
    // [+capabilities cap_net_raw=ep]

    while (std::getline(fs, line))
    {
        linenr++;
        line = strip(line);

        // don't know exactly why the dollar is also ignored, probably some
        // dark legacy.
        if (line.empty() || line[0] == '#' || line[0] == '$')
            continue;

        // an extra capability line that belongs to the context of the last
        // profile entry seen.
        if (line[0] == '+')
        {
            const auto good = parseExtraProfileLine(line, last_entry);

            if (!good)
            {
                badProfileLine(path, linenr, "lone capaility line, bad capability spec or bad +keyword");
            }

            continue;
        }

        splitWords(line, parts);

        if (parts.size() != 3)
        {
            badProfileLine(path, linenr, "invalid number of whitespace separated words");
            continue;
        }

        const auto &ownership = parts[1];
        std::string user, group;

        // split up user and group from {user}:{group} string. Two different
        // separator types are allowed like with `chmod`
        for (const auto sep: { ':', '.' })
        {
            const auto seppos = ownership.find_first_of(sep);
            if (seppos == ownership.npos)
                continue;

            user = ownership.substr(0, seppos);
            group = ownership.substr(seppos + 1);

            break;
        }

        if (user.empty() || group.empty())
        {
            badProfileLine(path, linenr, "bad user:group specification");
            continue;
        }

        const auto &location = parts[0];
        const auto &mode = parts[2];

        char *end = nullptr;
        mode_int = static_cast<mode_t>(std::strtoul(mode.c_str(), &end, 8));
        if (mode_int > 07777 || (end && *end != '\0'))
        {
            badProfileLine(path, linenr, "bad mode specification");
            continue;
        }

        auto &entry = addProfileEntry(location, user, group, mode_int);
        last_entry = &entry;
    }
}

bool Chkstat::checkHaveProc() const
{
    if (m_proc_mount_avail == ProcMountState::PROC_MOUNT_STATE_UNKNOWN)
    {
        m_proc_mount_avail = ProcMountState::PROC_MOUNT_STATE_UNAVAIL;
        char *pretend_no_proc = secure_getenv("CHKSTAT_PRETEND_NO_PROC");

        struct statfs proc;
        int r = statfs("/proc", &proc);
        if (!pretend_no_proc && r == 0 && proc.f_type == PROC_SUPER_MAGIC)
        {
            m_proc_mount_avail = ProcMountState::PROC_MOUNT_STATE_AVAIL;
        }
    }

    return m_proc_mount_avail == ProcMountState::PROC_MOUNT_STATE_AVAIL;
}

void Chkstat::printHeader()
{
    if (m_no_header.isSet())
        return;

    std::cout << "Checking permissions and ownerships - using the permissions files" << std::endl;

    for (const auto &profile_path: m_profile_paths)
    {
        std::cout << "\t" << profile_path << "\n";
    }

    if (!m_root_path.getValue().empty())
    {
        std::cout << "Using root " << m_root_path.getValue() << "\n";
    }
}

bool Chkstat::getCapabilities(const std::string &path, const std::string &label, ProfileEntry &entry, FileCapabilities &out)
{
    out.setFromFile(path);

    if (!out.valid())
    {
        if (!entry.hasCaps())
            return true;

        switch(errno)
        {
            default:
                break;
            // we get EBADF for files that don't support capabilities,
            // e.g. sockets or FIFOs
            case EBADF:
            {
                std::cerr << label << ": cannot assign capabilities for this kind of file" << std::endl;
                entry.caps.destroy();
                return false;
            }
            case EOPNOTSUPP:
            {
                std::cerr << label << ": no support for capabilities" << std::endl;
                entry.caps.destroy();
                return false;
            }
        }
    }

    if (entry.hasCaps())
    {
        // don't apply any set*id bits in case we apply capabilities
        // capabilities are the safer variant of set*id, the set*id bits
        // are only a fallback.
        entry.mode &= 0777;
    }

    return true;
}

int Chkstat::processEntries()
{
    FileStatus file_status;
    struct passwd *pwd = nullptr;
    struct group *grp = nullptr;
    uid_t uid;
    gid_t gid;
    FileDescGuard fd;
    size_t errors = 0;
    FileCapabilities caps;
    bool traversed_insecure;
    std::string fd_path;

    if (m_apply_changes.isSet() && !checkHaveProc())
    {
        std::cerr << "ERROR: /proc is not available - unable to fix policy violations." << std::endl;
        errors++;
        m_apply_changes.setValue(false);
    }

    for (auto &pair: m_profile_entries)
    {
        // these needs to be non-const currently, because further below the
        // capability logic is modifying entry properties on the fly.
        auto &entry = pair.second;
        const auto norm_path = entry.file.substr(m_root_path.getValue().length());

        if (!needToCheck(norm_path))
            continue;

        pwd = getpwnam(entry.owner.c_str());
        grp = getgrnam(entry.group.c_str());

        if (!pwd)
        {
            std::cerr << norm_path << ": unknown user " << entry.owner << ". ignoring entry." << std::endl;
            continue;
        }
        else if (!grp)
        {
            std::cerr << norm_path << ": unknown group " << entry.group << ". ignoring entry." << std::endl;
            continue;
        }

        uid = pwd->pw_uid;
        gid = grp->gr_gid;

        fd.set(safe_open(entry.file.c_str(), &file_status, uid, &traversed_insecure));

        if (!fd.valid())
            continue;
        else if (file_status.isLink())
            continue;

        // fd is opened with O_PATH, file operations like cap_get_fd() and fchown() don't work with it.
        //
        // We also don't want to do a proper open() of the file, since that doesn't even work for sockets
        // and might have side effects for pipes or devices.
        //
        // So we use path-based operations (yes!) with /proc/self/fd/xxx. (Since safe_open already resolved
        // all symlinks, 'fd' can't refer to a symlink which we'd have to worry might get followed.)
        if (checkHaveProc())
        {
            fd_path = std::string("/proc/self/fd/") + std::to_string(fd.get());
        }
        else
        {
            // fall back to plain path-access for read-only operation. (this much is fine)
            // below we make sure that in this case we report errors instead
            // of trying to fix policy violations insecurely
            fd_path = entry.file;
        }

        if (!getCapabilities(fd_path, norm_path, entry, caps))
        {
            errors++;
        }

        const bool perm_ok = (file_status.getModeBits()) == entry.mode;
        const bool owner_ok = file_status.matchesOwnership(uid, gid);
        const bool caps_ok = entry.caps == caps;

        if (perm_ok && owner_ok && caps_ok)
            // nothing to do
            continue;

        /*
         * first explain the differences between the encountered state of the
         * file and the desired state of the file
         */

        std::cout << norm_path << ": "
            << (m_apply_changes.isSet() ? "setting to " : "should be ")
            << entry.owner << ":" << entry.group << " "
            << FileModeInt(entry.mode);

        if (!caps_ok && entry.hasCaps())
        {
            std::cout << " \"" << entry.caps.toText() << "\"";
        }

        std::cout << ". (wrong";

        if (!owner_ok)
        {
            std::cout << " owner/group " << FileOwnership(file_status);
        }

        if (!perm_ok)
        {
            std::cout << " permissions " << FileModeInt(file_status.getModeBits());
        }

        if (!caps_ok)
        {
            if (!perm_ok || !owner_ok)
            {
                std::cout << ", ";
            }

            if (caps.valid())
            {
                std::cout << "capabilities \"" << caps.toText() << "\"";
            }
            else
            {
                std::cout << "missing capabilities";
            }
        }

        std::cout << ")" << std::endl;

        if (!m_apply_changes.isSet())
            continue;

        // don't allow high privileges for unusual file types
        if ((entry.hasCaps() || entry.hasSetXID()) && !file_status.isRegular() && !file_status.isDirectory())
        {
            std::cerr << norm_path << ": will only assign capabilities or setXid bits to regular files or directories" << std::endl;
            errors++;
            continue;
        }

        // don't give high privileges to files controlled by non-root users
        if (traversed_insecure)
        {
            if (entry.hasCaps() || (entry.mode & S_ISUID) || ((entry.mode & S_ISGID) && file_status.isRegular()))
            {
                std::cerr << norm_path << ": will not give away capabilities or setXid bits on an insecure path" << std::endl;
                errors++;
                continue;
            }
        }

        // only attempt to change the owner if we're actually privileged to do
        // so
        const bool change_owner = m_euid == 0 && !owner_ok;

        if (change_owner)
        {
            if (chown(fd_path.c_str(), uid, gid) != 0)
            {
                std::cerr << norm_path << ": chown: " << strerror(errno) << std::endl;
                errors++;
            }
        }

        // also re-apply the mode if we had to change ownership and a setXid
        // bit was set before, since this resets the setXid bit.
        if (!perm_ok || (change_owner && entry.hasSetXID()))
        {
            if (chmod(fd_path.c_str(), entry.mode) != 0)
            {
                std::cerr << norm_path << ": chmod: " << strerror(errno) << std::endl;
                errors++;
            }
        }

        // chown and - depending on the file system - chmod clear existing capabilities
        // so apply the intended caps even if they were correct previously
        if (entry.hasCaps() || !caps_ok)
        {
            if (file_status.isRegular())
            {
                // cap_set_file() tries to be helpful and does a lstat() to check that it isn't called on
                // a symlink. So we have to open() it (without O_PATH) and use cap_set_fd().
                FileDescGuard cap_fd( open(fd_path.c_str(), O_NOATIME | O_CLOEXEC | O_RDONLY) );
                if (!cap_fd.valid())
                {
                    std::cerr << norm_path << ": open() for changing capabilities: " << strerror(errno) << std::endl;
                    errors++;
                }
                else if (!entry.caps.applyToFD(cap_fd.get()))
                {
                    // ignore ENODATA when clearing caps - it just means there were no caps to remove
                    if (errno != ENODATA || entry.hasCaps())
                    {
                        std::cerr << norm_path << ": cap_set_fd: " << strerror(errno) << std::endl;
                        errors++;
                    }
                }
            }
            else
            {
                std::cerr << norm_path << ": cannot set capabilities: not a regular file" << std::endl;
                errors++;
            }
        }
    }

    if (errors)
    {
        std::cerr << "ERROR: not all operations were successful." << std::endl;
        return 1;
    }

    return 0;
}

int Chkstat::run()
{
    if (!validateArguments())
    {
        return 2;
    }

    if (!processArguments())
    {
        return 1;
    }

    if (m_system_mode.isSet())
    {
        parseSysconfig();
        if (m_force_level_list.isSet())
        {
            std::istringstream ss(m_force_level_list.getValue());
            std::string line;
            while (std::getline(ss, line))
            {
                addProfile(line);
            }
        }

        if (m_profiles.empty())
        {
            addProfile("secure");
        }

        // always add the local profile
        addProfile("local");

        for (const auto &path: m_input_args.getValue())
        {
            m_checklist.insert(path);
        }

        collectProfiles();
    }
    else
    {
        // only process the profiles specified on the command line
        appendContainer(m_profile_paths, m_input_args.getValue());
    }

    // apply possible command line overrides to force en-/disable fscaps
    if (m_force_fscaps.isSet())
    {
        m_use_fscaps = true;
    }
    else if (m_disable_fscaps.isSet())
    {
        m_use_fscaps = false;
    }

    if (m_use_fscaps && !checkFsCapsSupport())
    {
        std::cerr << "Warning: running kernel does not support fscaps" << std::endl;
    }

    for (const auto &profile_path: m_profile_paths)
    {
        parseProfile(profile_path);
    }


    // check whether explicitly listed files are actually configured in
    // profiles
    for( const auto &path: m_checklist )
    {
        // TODO: both here and in needToCheck() the command line arguments are
        // not checked for trailing slashes. For directories the profile
        // entries require trailing slashes but if a user enters an explicit
        // path to a directory without the trailing slash then it won't be
        // recognized. This was this way in the original code already.

        auto full_path = path;

        // we need to add a potential alternative root directory, since
        // addProfileEntry stores entries using the full path.
        if (!m_root_path.getValue().empty())
        {
            full_path = m_root_path.getValue() + '/' + path;
        }

        if (m_profile_entries.find(full_path) == m_profile_entries.end())
        {
            std::cerr << path << ": no configuration entry in active permission profiles found. Cannot check this path." << std::endl;
        }
    }

    printHeader();

    return processEntries();
}

int main(int argc, const char **argv)
{
    Chkstat chkstat(argc, argv);
    return chkstat.run();
}

// vim: et ts=4 sts=4 sw=4 :
