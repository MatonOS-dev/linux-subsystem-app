#!/usr/bin/env python3
"""A GL merge-dir manifest must canonicalize identically through share aliases."""
import os
import pathlib
import re
import shutil
import subprocess
import hashlib
import stat
import struct

base = pathlib.Path(__file__).resolve().parents[1]
root = base / "build/icd-dup-host"
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)

def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)

def output(*args, **kwargs):
    return subprocess.check_output(list(map(str, args)), **kwargs)

def bundle(name, files):
    source = root / (name + "-src")
    source.mkdir()
    for path, target in files.items():
        dest = source / path.lstrip("/")
        dest.parent.mkdir(parents=True, exist_ok=True)
        if target is None:
            dest.write_text("{}\n")
        else:
            dest.symlink_to(target)
    packed = root / (name + "-bundle")
    packed.mkdir()
    digest=hashlib.sha256(name.encode()).hexdigest(); split="pack_"+digest
    image=packed/split;shutil.copytree(source,image,symlinks=True)
    rows=[]
    for path in sorted(image.rglob("*"),key=lambda p:p.relative_to(image).as_posix().encode()):
        rel="/"+path.relative_to(image).as_posix();st=path.lstat()
        if stat.S_ISDIR(st.st_mode):kind,target,layer,size="dir","",0xffffffff,0
        elif stat.S_ISLNK(st.st_mode):kind,target,layer,size="symlink",os.readlink(path),0xffffffff,len(os.readlink(path).encode())
        else:kind,target,layer,size="file","",0,st.st_size
        checksum=hashlib.sha256((rel+kind+target).encode()).hexdigest()
        rows.append((rel,checksum,kind,target,layer,size,int(st.st_mtime),st.st_uid,st.st_gid,st.st_mode,st.st_ino))
    rows.insert(0,("/",hashlib.sha256(b"root").hexdigest(),"dir","",0xffffffff,0,0,0,0,stat.S_IFDIR|0o755,0))
    def string(value):
        data=value.encode();return struct.pack("<I",len(data))+data
    index=bytearray(b"MTPKIDX3"+struct.pack("<I",1)+string(split)+string("assets/matonos/pack.erofs")+struct.pack("<Q",1)+bytes.fromhex(digest)+struct.pack("<I",len(rows)))
    for path,checksum,kind,target,layer,size,mtime,uid,gid,mode,inode in rows:
        for value in (path,checksum,kind,target):index+=string(value)
        index+=struct.pack("<I",0)+struct.pack("<IQQIIIIQ",layer,size,mtime,uid,gid,mode,0,inode)
    (packed/"layers.index").write_bytes(index)
    return source, packed


gl = "/usr/lib/x86_64-linux-gnu/GL"
manifest = "maton_icd.json"
runtime_files = {
    # bwrap needs the nested mountpoint to exist in the bound runtime tree.
    "/lib/x86_64-linux-gnu/GL/.mountpoint": None,
    "/share/vulkan/icd.d": gl + "/vulkan/icd.d",
}
app_files = {
    "/share/app-info/example.xml": None,
    # This is the real VulkanInfo app deployment alias, not an ICD copy.
    "/share/vulkan/icd.d": gl + "/vulkan/icd.d",
}
default_files = {
    "/vulkan/icd.d/" + manifest: gl + "/maton/vulkan/icd.d/" + manifest,
    "/share/vulkan/explicit_layer.d/VkLayer_fixture.json": None,
}
ext_files = {"/vulkan/icd.d/" + manifest: None}
runtime_src, runtime = bundle("runtime", runtime_files)
app_src, app = bundle("app", app_files)
default_src, default = bundle("default", default_files)
extension_src, extension = bundle("extension", ext_files)

probe_src = root / "probe.c"
probe_src.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    const char *paths[] = {
        "/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d/maton_icd.json",
        "/usr/share/vulkan/icd.d/maton_icd.json",
        "/app/share/vulkan/icd.d/maton_icd.json",
    };
    char expected[PATH_MAX], resolved[3][PATH_MAX], link[PATH_MAX];
    struct stat gl_stat, share_stat, app_stat, link_stat;
    if (!realpath("/usr/lib/x86_64-linux-gnu/GL/maton/vulkan/icd.d/maton_icd.json", expected)) {
        perror("realpath expected GL extension"); return 1;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!realpath(paths[i], resolved[i])) { perror(paths[i]); return 2; }
        if (strcmp(resolved[i], expected)) {
            fprintf(stderr, "%s => %s, expected %s\n", paths[i], resolved[i], expected);
            return 3;
        }
        int fd = open(paths[i], O_PATH);
        if (fd < 0) { perror(paths[i]); return 18; }
        char proc[64], via_fd[PATH_MAX];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
        if (!realpath(proc, via_fd) || strcmp(via_fd, expected)) {
            fprintf(stderr, "%s => %s via proc fd, expected %s\n", paths[i], via_fd, expected);
            return 19;
        }
        close(fd);
        ssize_t n = readlink(paths[i], link, sizeof(link) - 1);
        if (n < 0 && errno != EINVAL) { perror("readlink"); return 4; }
    }
    if (strcmp(resolved[0], resolved[1]) || strcmp(resolved[0], resolved[2])) return 5;
    if (stat(paths[0], &gl_stat) || stat(paths[1], &share_stat) ||
        stat(paths[2], &app_stat) || gl_stat.st_dev != share_stat.st_dev ||
        gl_stat.st_ino != share_stat.st_ino || gl_stat.st_dev != app_stat.st_dev ||
        gl_stat.st_ino != app_stat.st_ino) {
        fprintf(stderr, "symlink aliases do not share target identity\n"); return 8;
    }
    if (lstat(paths[1], &link_stat) || !S_ISLNK(link_stat.st_mode)) {
        fprintf(stderr, "/usr/share manifest is not exposed as its source symlink\n"); return 9;
    }
    ssize_t n = readlink(paths[1], link, sizeof(link) - 1);
    if (n < 0) { perror("readlink /usr/share manifest"); return 10; }
    link[n] = 0;
    if (strcmp(link, "../../default/vulkan/icd.d/maton_icd.json")) {
        fprintf(stderr, "readlink returned %s\n", link); return 11;
    }
    DIR *dir = opendir("/usr/share/vulkan/icd.d");
    if (!dir) { perror("opendir /usr/share/vulkan/icd.d"); return 12; }
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (!strcmp(entry->d_name, "maton_icd.json")) {
        if (entry->d_type != DT_LNK) { fprintf(stderr, "share d_type=%u, expected symlink\n", entry->d_type); return 13; }
        found++;
    }
    closedir(dir);
    if (found != 1) { fprintf(stderr, "share manifest entry count=%d\n", found); return 14; }
    struct stat gl_dir_stat, share_dir_stat, app_dir_stat;
    if (stat("/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d", &gl_dir_stat) ||
        stat("/usr/share/vulkan/icd.d", &share_dir_stat) ||
        stat("/app/share/vulkan/icd.d", &app_dir_stat) ||
        gl_dir_stat.st_dev != share_dir_stat.st_dev || gl_dir_stat.st_ino != share_dir_stat.st_ino ||
        gl_dir_stat.st_dev != app_dir_stat.st_dev || gl_dir_stat.st_ino != app_dir_stat.st_ino) {
        fprintf(stderr, "GL, runtime and app merge-dir directory identities differ\n"); return 20;
    }
    if (lstat("/app/share/vulkan/icd.d", &link_stat) || !S_ISLNK(link_stat.st_mode)) {
        fprintf(stderr, "/app merge-dir alias is not a symlink\n"); return 21;
    }
    n = readlink("/app/share/vulkan/icd.d", link, sizeof(link) - 1);
    if (n < 0) { perror("readlink /app merge-dir alias"); return 22; }
    link[n] = 0;
    if (strcmp(link, "/usr/lib/x86_64-linux-gnu/GL/vulkan/icd.d")) {
        fprintf(stderr, "/app merge-dir readlink returned %s\n", link); return 23;
    }
    if (lstat(paths[2], &link_stat) || !S_ISLNK(link_stat.st_mode)) {
        fprintf(stderr, "/app/share manifest is not exposed as its source symlink\n"); return 15;
    }
    n = readlink(paths[2], link, sizeof(link) - 1);
    if (n < 0) { perror("readlink /app/share manifest"); return 16; }
    link[n] = 0;
    if (strcmp(link, "../../default/vulkan/icd.d/maton_icd.json")) {
        fprintf(stderr, "/app manifest readlink returned %s\n", link); return 17;
    }
    errno = 0;
    if (realpath("/app/share/vulkan/explicit_layer.d/VkLayer_fixture.json", link) || errno != ENOENT) {
        fprintf(stderr, "/app unexpectedly exposes a GL layer: %s errno=%d\n", link, errno);
        return 7;
    }
    puts("PASS GL.maton, /usr/share and /app/share aliases have Linux symlink identity");
    return 0;
}
''')
probe = root / "probe"
run("cc", "-static", "-O2", "-Wall", "-Wextra", "-Werror", probe_src, "-o", probe)

# Each share path is a symlink alias, as in a Flatpak runtime. The extension
# itself is mounted at GL/maton and the GL merge-dir links to its contents.
gl_dir = gl
def args(target,bundle):return ["--erofs-mount",target,bundle/"layers.index",bundle]
proxy = [base / "build/linux-layer-v3", "--android-filter",
         *args("/usr",runtime),*args("/app",app),*args(gl_dir+"/default",default),*args(gl_dir+"/maton",extension)]
# Exercise absolute runtime symlinks and the GL merge-dir after bwrap pivots
# into its new root, as the installed Flatpak launcher does.
bwrap_path = shutil.which("bwrap")
if bwrap_path:
    bwrap = root / "bwrap"
    shutil.copy2(bwrap_path, bwrap)
    glroot = "/usr/lib/x86_64-linux-gnu/GL"
    runtime_fd = os.open(runtime_src, os.O_RDONLY | os.O_DIRECTORY)
    app_fd = os.open(app_src, os.O_RDONLY | os.O_DIRECTORY)
    default_fd = os.open(default_src, os.O_RDONLY | os.O_DIRECTORY)
    extension_fd = os.open(extension_src, os.O_RDONLY | os.O_DIRECTORY)
    argv = [base / "build/linux-layer-v3", "--android-filter",
            "--map", pathlib.Path("/lib").resolve(), "/lib",
            "--map", pathlib.Path("/lib64").resolve(), "/lib64",
            *args("/usr",runtime),*args("/app",app),*args(glroot+"/default",default),*args(glroot+"/maton",extension),
            bwrap, "--ro-bind-fd", str(runtime_fd), "/usr",
            "--ro-bind-fd", str(app_fd), "/app", "--tmpfs", glroot,
            "--ro-bind-fd", str(default_fd), glroot + "/default",
            "--ro-bind-fd", str(extension_fd), glroot + "/maton",
            "--dir", glroot + "/vulkan/icd.d",
            "--symlink", "../../default/vulkan/icd.d/maton_icd.json",
            glroot + "/vulkan/icd.d/maton_icd.json",
            "--proc", "/proc", "--dev", "/dev", "--ro-bind", probe, "/probe",
            "/probe"]
    try:
        rooted = output(*argv, pass_fds=[runtime_fd, app_fd, default_fd, extension_fd], timeout=30).decode()
    finally:
        for fd in (runtime_fd, app_fd, default_fd, extension_fd):
            os.close(fd)
    assert "PASS GL.maton" in rooted, rooted
    print("PASS Vulkan manifest realpath aliases through bwrap pivot", flush=True)
else:
    print("SKIP bwrap pivot alias test: bwrap unavailable", flush=True)

# Verify the loader's actual discovery behavior: separate manifest pathnames
# load the same ICD twice even when the OS resolves them to one file. The
# launcher must therefore constrain discovery to Flatpak's GL merge-dir.
vulkaninfo = shutil.which("vulkaninfo")
lvp = pathlib.Path("/usr/share/vulkan/icd.d/lvp_icd.json")
if vulkaninfo and lvp.is_file():
    gl_icd = root / "gl-icd"
    gl_icd.mkdir()
    shutil.copy2(lvp, gl_icd / lvp.name)
    alias = root / "share-alias.json"
    alias.symlink_to(gl_icd / lvp.name)
    assert os.path.samefile(gl_icd / lvp.name, alias)

    duplicate_env = dict(os.environ,
                         VK_DRIVER_FILES=f"{gl_icd / lvp.name}:{alias}",
                         VK_LOADER_DEBUG="error")
    duplicate = output(vulkaninfo, "--summary", env=duplicate_env,
                       stderr=subprocess.STDOUT, timeout=45).decode()
    assert re.search(r"^GPU0:", duplicate, re.MULTILINE), duplicate
    assert re.search(r"^GPU1:", duplicate, re.MULTILINE), duplicate

    single_env = dict(os.environ, VK_DRIVER_FILES=str(gl_icd),
                      VK_LOADER_DEBUG="error")
    single = output(vulkaninfo, "--summary", env=single_env,
                    stderr=subprocess.STDOUT, timeout=45).decode()
    assert re.search(r"^GPU0:", single, re.MULTILINE), single
    assert not re.search(r"^GPU1:", single, re.MULTILINE), single
    print("PASS Vulkan loader repeats a symlinked ICD path; one GL directory loads it once", flush=True)
else:
    print("SKIP Vulkan loader alias integration: vulkaninfo or lavapipe unavailable", flush=True)
