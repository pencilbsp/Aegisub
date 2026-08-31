#!/usr/bin/env python3

import re
import sys
import os
import glob
import shutil
import stat
import subprocess

is_bad_lib = re.compile(r'(/usr/local|/opt)').match
is_sys_lib = re.compile(r'(/usr|/System)').match
otool_libname_extract = re.compile(r'\s+([/@].*?)[(\s:]').search
goodlist = []
badlist = []
badlist_orig = []
link_map = {}
change_map = {}
target_map = {}


def find_missing_lib(lib, targetdir):
    if not is_bad_lib(lib):
        return None

    build_dir = os.path.abspath(os.path.join(targetdir, "..", "..", ".."))
    basename = os.path.basename(lib)
    for root, _, files in os.walk(os.path.join(build_dir, "subprojects")):
        if basename in files:
            return os.path.join(root, basename)

    for prefix in ("/opt/homebrew", "/usr/local"):
        opt_prefix = os.path.join(prefix, "opt") + os.sep
        if not lib.startswith(opt_prefix):
            continue

        rest = lib[len(opt_prefix):]
        formula, _, relative_path = rest.partition(os.sep)
        if not formula or not relative_path:
            continue

        pattern = os.path.join(prefix, "Cellar", formula, "*", relative_path)
        for candidate in sorted(glob.glob(pattern), reverse=True):
            if os.path.exists(candidate):
                return candidate

    return None


def otool(cmdline):
    with subprocess.Popen(['otool'] + cmdline, stdout=subprocess.PIPE,
                          encoding='utf-8') as p:
        return p.stdout.readlines()


def get_rpath(lib):
    info = otool(['-l', lib])
    commands = []
    command = []
    for line in info:
        line = line.strip()
        if line.startswith("Load command "):
            commands.append(command)
            command = []
        else:
            command.append(line)
    commands.append(command)

    # yuck
    return [line.split()[1] for command in commands if "cmd LC_RPATH" in command for line in command if line.startswith("path")]


def resolve_loader_path(lib, path):
    if path.startswith("@loader_path/"):
        return os.path.join(os.path.dirname(lib), path[len("@loader_path/"):])
    return path


def resolve_executable_path(targetdir, path):
    if path.startswith("@executable_path/"):
        return os.path.join(targetdir, path[len("@executable_path/"):])
    return path


def collectlibs(lib, masterlist, targetdir):
    global goodlist, link_map, change_map, target_map
    liblist = otool(['-L', lib])
    locallist = []

    for l in liblist:
        l = otool_libname_extract(l)
        if not l:
            continue

        l = l.group(1)
        l_orig = l
        copydir = targetdir

        if os.path.basename(l) == os.path.basename(lib):
            continue

        if l.startswith("@rpath/"):
            rpath = get_rpath(lib)

            if not rpath:
                print(f"{lib} uses @rpath but has no rpath set!")
                exit(-1)

            rpath_dir = rpath[0]
            for candidate_dir in rpath:
                candidate = os.path.join(candidate_dir, l[len("@rpath/"):])
                candidate = resolve_loader_path(lib, candidate)
                candidate = resolve_executable_path(targetdir, candidate)
                if os.path.exists(candidate):
                    rpath_dir = candidate_dir
                    break

            l = os.path.join(rpath_dir, l[len("@rpath/"):])
            if rpath_dir == "@loader_path/data":
                copydir = os.path.join(targetdir, rpath_dir[len("@loader_path/"):])

        l = resolve_loader_path(lib, l)
        l = resolve_executable_path(targetdir, l)

        if is_bad_lib(l):
            if l not in badlist:
                badlist.append(l)
            if l_orig not in badlist_orig:
                badlist_orig.append(l_orig)
        if ((not is_sys_lib(l)) or is_bad_lib(l)) and l not in masterlist:
            locallist.append(l)
            print("found %s:" % l)

            check = l
            link_list = []
            while check:
                basename = os.path.basename(check)
                os.makedirs(copydir, exist_ok=True)
                target = os.path.join(copydir, basename)

                if os.path.islink(target):
                    # If a library was a symlink to a file with the same name in another directory,
                    # this could otherwise cause a FileNotFoundError
                    os.remove(target)

                if os.path.isfile(check) and not os.path.islink(check):
                    target_rel = os.path.relpath(target, targetdir)
                    if os.path.exists(target) and os.path.samefile(check, target):
                        print("    FILE %s ... already in target" % check)
                        target_map[check] = target_rel
                        change_map[l_orig] = target_rel
                        if link_list:
                            for link in link_list:
                                link_map[link] = basename
                        break

                    try:
                        shutil.copy(check, target)
                    except PermissionError:
                        print("    FILE %s ... skipped" % check)
                        break
                    print("    FILE %s ... copied to target" % check)
                    target_map[check] = target_rel
                    change_map[l_orig] = target_rel
                    if link_list:
                        for link in link_list:
                            link_map[link] = basename
                    break

                if os.path.islink(check):
                    link_dst = os.readlink(check)
                    try:
                        os.symlink(link_dst, target)
                    except FileExistsError:
                        print("    LINK %s ... existed" % check)
                        break
                    print("    LINK %s ... copied to target" % check)
                    target_map[check] = os.path.relpath(target, targetdir)
                    link_list.append(basename)
                    check = os.path.join(os.path.dirname(check), link_dst)
                    continue

                replacement = find_missing_lib(check, targetdir)
                if replacement:
                    print("    MISSING %s ... using %s" % (check, replacement))
                    locallist[-1] = replacement
                    check = replacement
                    continue

                print("    MISSING %s ... skipped" % check)
                locallist.pop()
                break
        elif l not in goodlist and l not in masterlist:
            goodlist.append(l)
    masterlist.extend(locallist)

    for l in locallist:
        collectlibs(l, masterlist, targetdir)


if __name__ == '__main__':
    binname = sys.argv[1]
    targetdir = os.path.dirname(binname)
    print("Searching for libraries in", binname, "...")
    libs = [binname]
    collectlibs(binname, libs, targetdir)

    print()
    print("System libraries used...")
    goodlist.sort()
    for l in goodlist:
        print(l)

    print()
    print("Fixing library install names...")
    in_tool_cmdline = ['install_name_tool']
    all_changes = dict(change_map)
    for lib in badlist_orig:
        if lib not in all_changes:
            libbase = os.path.basename(lib)
            all_changes[lib] = link_map.get(libbase, libbase)

    for lib, target_rel in all_changes.items():
        libbase = os.path.basename(target_rel)
        if libbase in link_map:
            target_rel = os.path.join(os.path.dirname(target_rel), link_map[libbase])
            print("%s -> @executable_path/%s (REMAPPED)" % (lib, target_rel))
        else:
            print("%s -> @executable_path/%s" % (lib, target_rel))
        in_tool_cmdline = in_tool_cmdline + ['-change', lib,
                                             '@executable_path/' + target_rel]
    for lib in libs:
        target_rel = target_map.get(lib, os.path.basename(lib))
        libbase = os.path.basename(target_rel)
        targetlib = os.path.join(targetdir, target_rel)
        orig_permission = os.stat(targetlib).st_mode
        if not(orig_permission & stat.S_IWUSR):
            os.chmod(targetlib, orig_permission | stat.S_IWUSR)
        subprocess.run(in_tool_cmdline + ['-id', '@executable_path/' + target_rel,
                                          targetlib])
        if not(orig_permission & stat.S_IWUSR):
            os.chmod(targetlib, orig_permission)

    if badlist:
        print()
        print("WARNING: The following libraries have blacklisted paths:")
        for lib in sorted(badlist):
            print(lib)
        print(
            "These paths normally have files from a package manager, which means that end result may not work if copied to another machine.")

    print()
    print("All done!")
