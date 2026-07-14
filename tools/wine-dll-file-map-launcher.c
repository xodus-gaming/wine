#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#include <sys/mman.h>

static void usage( const char *argv0 )
{
    fprintf( stderr,
             "Usage: %s <wine-binary> <source-file> <nt-name> [--] [wine-args...]\n"
             "\n"
             "Creates anonymous backing from <source-file>, appends one WINE_DLL_FILE_MAP entry,\n"
             "then execs <wine-binary>. If no wine-args are provided, <nt-name> is passed as\n"
             "the target executable argument.\n",
             argv0 );
}

static bool debug_enabled( void )
{
    const char *value = getenv( "WINE_DLL_FILE_MAP_DEBUG" );
    return value && value[0] && strcmp( value, "0" );
}

static void debug_log( const char *message )
{
    if (debug_enabled()) fprintf( stderr, "wine-dll-file-map-launcher: %s\n", message );
}

struct backing_store
{
    int fd;
    bool needs_detach;
    char device[PATH_MAX];
    char mount_dir[PATH_MAX];
};

#ifdef __linux__
static int create_memfd( void )
{
    return syscall( SYS_memfd_create, "wine-dll-file-map", MFD_CLOEXEC );
}
#endif

static int wait_for_child( pid_t pid )
{
    int status;

    while (waitpid( pid, &status, 0 ) == -1)
    {
        if (errno == EINTR) continue;
        return -1;
    }

    return status;
}

static int run_command( char *const argv[] )
{
    pid_t pid = fork();
    int status;

    if (pid == -1) return -1;
    if (!pid)
    {
        execv( argv[0], argv );
        _exit( 127 );
    }

    status = wait_for_child( pid );
    if (status == -1) return -1;
    if (!WIFEXITED( status ) || WEXITSTATUS( status )) return -1;
    return 0;
}

static int run_command_capture( char *const argv[], char *buffer, size_t size )
{
    int pipefd[2];
    pid_t pid;
    ssize_t ret;
    size_t used = 0;
    int status;

    if (size) buffer[0] = 0;
    if (pipe( pipefd ) == -1) return -1;

    pid = fork();
    if (pid == -1)
    {
        close( pipefd[0] );
        close( pipefd[1] );
        return -1;
    }

    if (!pid)
    {
        dup2( pipefd[1], STDOUT_FILENO );
        close( pipefd[0] );
        close( pipefd[1] );
        execv( argv[0], argv );
        _exit( 127 );
    }

    close( pipefd[1] );
    while ((ret = read( pipefd[0], buffer + used, size > used ? size - used - 1 : 0 )) > 0)
    {
        used += ret;
        if (used + 1 >= size) break;
    }
    close( pipefd[0] );

    if (size) buffer[used < size ? used : size - 1] = 0;
    status = wait_for_child( pid );
    if (status == -1) return -1;
    if (!WIFEXITED( status ) || WEXITSTATUS( status )) return -1;
    return 0;
}

static int run_command_status( char *const argv[] )
{
    pid_t pid = fork();
    int status;

    if (pid == -1) return -1;
    if (!pid)
    {
        execv( argv[0], argv );
        _exit( 127 );
    }

    status = wait_for_child( pid );
    if (status == -1) return -1;
    return status;
}

#ifdef __APPLE__
static void log_detach_holders( const struct backing_store *backing )
{
    char output[8192];
    char *device_argv[] = {"/usr/sbin/lsof", (char *)backing->device, NULL};
    char *mount_argv[] = {"/usr/sbin/lsof", "+D", (char *)backing->mount_dir, NULL};

    if (backing->device[0] && run_command_capture( device_argv, output, sizeof(output) ) == 0 && output[0])
    {
        debug_log( "detach blockers on device:" );
        if (debug_enabled()) fputs( output, stderr );
    }
    if (backing->mount_dir[0] && run_command_capture( mount_argv, output, sizeof(output) ) == 0 && output[0])
    {
        debug_log( "detach blockers under mount:" );
        if (debug_enabled()) fputs( output, stderr );
    }
}

static int create_ramdisk_backing( off_t source_size, struct backing_store *backing )
{
    char sectors[32];
    char mount_dir[PATH_MAX];
    char mkdir_template[PATH_MAX];
    char output[4096];
    char template[PATH_MAX];
    char volume[64];
    char *attach_argv[] = {"/usr/bin/hdiutil", "attach", "-nomount", NULL, NULL};
    char *format_argv[] = {"/sbin/newfs_hfs", "-v", volume, NULL, NULL};
    char *mount_argv[] = {"/sbin/mount", "-t", "hfs", "-o", "nobrowse", NULL, mount_dir, NULL};
    off_t target_size = source_size + (32 * 1024 * 1024);
    unsigned long long sector_count;
    int status;
    int fd;

    if (target_size < (64 * 1024 * 1024)) target_size = 64 * 1024 * 1024;
    sector_count = (unsigned long long)((target_size + 511) / 512);
    snprintf( sectors, sizeof(sectors), "ram://%llu", sector_count );
    snprintf( volume, sizeof(volume), "wdfm-%d", (int)getpid() );
    snprintf( mkdir_template, sizeof(mkdir_template), "%s/%s", "/private/tmp", "wdfm-mnt-XXXXXX" );
    if (!mkdtemp( mkdir_template ))
    {
        debug_log( "backing store: mkdtemp mountpoint failed" );
        return -1;
    }
    snprintf( mount_dir, sizeof(mount_dir), "%s", mkdir_template );
    snprintf( backing->mount_dir, sizeof(backing->mount_dir), "%s", mount_dir );
    attach_argv[3] = sectors;

    if (run_command_capture( attach_argv, output, sizeof(output) ) == -1)
    {
        rmdir( mount_dir );
        debug_log( "backing store: hdiutil attach failed" );
        if (!errno) errno = EIO;
        return -1;
    }

    if (sscanf( output, "%1023s", backing->device ) != 1)
    {
        rmdir( mount_dir );
        errno = EIO;
        return -1;
    }

    format_argv[3] = backing->device;
    status = run_command_status( format_argv );
    if (status == -1 || !WIFEXITED( status ) || WEXITSTATUS( status ))
    {
        debug_log( "backing store: newfs_hfs failed" );
        errno = EIO;
        return -1;
    }

    mount_argv[5] = backing->device;
    status = run_command_status( mount_argv );
    if (status == -1 || !WIFEXITED( status ) || WEXITSTATUS( status ))
    {
        debug_log( "backing store: mount failed" );
        errno = EIO;
        return -1;
    }

    snprintf( template, sizeof(template), "%s/%s", mount_dir, "wine-dll-file-map-XXXXXX" );
    fd = mkstemp( template );
    if (fd == -1) return -1;
    if (unlink( template ) == -1)
    {
        close( fd );
        return -1;
    }

    backing->fd = fd;
    backing->needs_detach = true;
    debug_log( "backing store: hdiutil ramdisk + mkstemp + unlink" );
    return 0;
}

static int create_shared_backing( off_t source_size, struct backing_store *backing )
{
    memset( backing, 0, sizeof(*backing) );
    backing->fd = -1;

    if (create_ramdisk_backing( source_size, backing ) == -1) return -1;
    return 0;
}
#else
static int create_shared_backing( off_t source_size, struct backing_store *backing )
{
    (void)source_size;
    memset( backing, 0, sizeof(*backing) );
    backing->fd = -1;

#ifdef __linux__
    backing->fd = create_memfd();
    if (backing->fd != -1)
    {
        debug_log( "backing store: memfd_create" );
        return 0;
    }
#endif

#ifdef PATH_MAX
    {
        const char *tmpdir = getenv( "TMPDIR" );
        char path[PATH_MAX];

        if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
        snprintf( path, sizeof(path), "%s/%s", tmpdir, "wine-dll-file-map-XXXXXX" );
        backing->fd = mkstemp( path );
        if (backing->fd == -1) return -1;
        unlink( path );
        debug_log( "backing store: mkstemp + unlink" );
        return 0;
    }
#endif
}
#endif

static int cleanup_backing( struct backing_store *backing )
{
    if (backing->fd != -1) close( backing->fd );

#ifdef __APPLE__
    if (backing->needs_detach)
    {
        char *umount_argv[] = {"/sbin/umount", backing->mount_dir, NULL};
        char *force_umount_argv[] = {"/sbin/umount", "-f", backing->mount_dir, NULL};
        char *detach_argv[] = {"/usr/bin/hdiutil", "detach", backing->device, NULL};
        char *force_detach_argv[] = {"/usr/bin/hdiutil", "detach", "-force", backing->device, NULL};
        int i;

        for (i = 0; i < 20; i++)
        {
            if (run_command( umount_argv ) == 0) break;
            usleep( 250000 );
        }
        if (i == 20 && run_command( force_umount_argv ) == -1)
        {
            log_detach_holders( backing );
            debug_log( "backing store: umount failed" );
            return -1;
        }
        if (i == 20) debug_log( "backing store: umount forced" );

        for (i = 0; i < 20; i++)
        {
            if (run_command( detach_argv ) == 0) break;
            usleep( 250000 );
        }
        if (i == 20 && run_command( force_detach_argv ) == -1)
        {
            log_detach_holders( backing );
            debug_log( "backing store: hdiutil detach failed" );
            return -1;
        }
        if (i == 20) debug_log( "backing store: hdiutil detach forced" );
        if (backing->mount_dir[0]) rmdir( backing->mount_dir );
    }
#endif

    return 0;
}

static int copy_file_to_fd( const char *src_path, int dst_fd )
{
    char buffer[65536];
    ssize_t read_ret;
    int src_fd = open( src_path, O_RDONLY );
    struct stat st;
    void *mapping = MAP_FAILED;

    if (src_fd == -1)
    {
        perror( "open source-file" );
        return -1;
    }

    if (fstat( src_fd, &st ) == -1)
    {
        perror( "fstat source-file" );
        close( src_fd );
        return -1;
    }

    if (ftruncate( dst_fd, st.st_size ) == -1)
    {
        perror( "ftruncate shared backing" );
        close( src_fd );
        return -1;
    }

    if (st.st_size > 0)
    {
        mapping = mmap( NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0 );
        if (mapping != MAP_FAILED)
        {
            size_t offset = 0;

            debug_log( "copy path: mmap shared backing" );
            while ((read_ret = read( src_fd, (char *)mapping + offset, st.st_size - offset )) > 0)
                offset += read_ret;

            if (read_ret == -1)
            {
                perror( "read source-file" );
                munmap( mapping, st.st_size );
                close( src_fd );
                return -1;
            }
        }
        else
        {
            debug_log( "copy path: write shared backing fallback" );
            while ((read_ret = read( src_fd, buffer, sizeof(buffer) )) > 0)
            {
                ssize_t written = 0;
                while (written < read_ret)
                {
                    ssize_t write_ret = write( dst_fd, buffer + written, read_ret - written );
                    if (write_ret == -1)
                    {
                        perror( "write shared backing" );
                        close( src_fd );
                        return -1;
                    }
                    written += write_ret;
                }
            }

            if (read_ret == -1)
            {
                perror( "read source-file" );
                close( src_fd );
                return -1;
            }
        }
    }

    close( src_fd );
    if (mapping != MAP_FAILED && msync( mapping, st.st_size, MS_SYNC ) == -1)
    {
        perror( "msync shared backing" );
        munmap( mapping, st.st_size );
        return -1;
    }
    if (mapping != MAP_FAILED) munmap( mapping, st.st_size );
    return 0;
}

static int clear_cloexec( int fd )
{
    int flags = fcntl( fd, F_GETFD );
    if (flags == -1)
    {
        perror( "fcntl(F_GETFD)" );
        return -1;
    }
    if (fcntl( fd, F_SETFD, flags & ~FD_CLOEXEC ) == -1)
    {
        perror( "fcntl(F_SETFD)" );
        return -1;
    }
    return 0;
}

static char *build_env_value( const char *existing, int fd, const char *nt_name )
{
    int len = snprintf( NULL, 0, "%s%s%d:%s",
                        existing && *existing ? existing : "",
                        existing && *existing ? "|" : "",
                        fd, nt_name );
    char *ret = malloc( len + 1 );

    if (!ret) return NULL;

    snprintf( ret, len + 1, "%s%s%d:%s",
              existing && *existing ? existing : "",
              existing && *existing ? "|" : "",
              fd, nt_name );
    return ret;
}

static const char *default_wine_target( const char *nt_name )
{
    if (!strncmp( nt_name, "\\??\\", 4 )) return nt_name + 4;
    return nt_name;
}

int main( int argc, char **argv )
{
    const char *wine_binary, *source_file, *nt_name;
    const char *wine_target;
    const char *existing_env;
    struct stat st;
    struct backing_store backing;
    char *env_value;
    char **child_argv;
    int i, child_argc, status;
    pid_t pid;

    if (argc < 4)
    {
        usage( argv[0] );
        return 2;
    }

    wine_binary = argv[1];
    source_file = argv[2];
    nt_name = argv[3];
    wine_target = default_wine_target( nt_name );

    if (stat( source_file, &st ) == -1)
    {
        perror( "stat source-file" );
        return 1;
    }

    if (create_shared_backing( st.st_size, &backing ) == -1)
    {
        perror( "create shared backing" );
        return 1;
    }

    if (copy_file_to_fd( source_file, backing.fd ) == -1) return 1;
    if (clear_cloexec( backing.fd ) == -1) return 1;

    existing_env = getenv( "WINE_DLL_FILE_MAP" );
    env_value = build_env_value( existing_env, backing.fd, nt_name );
    if (!env_value)
    {
        fprintf( stderr, "failed to allocate WINE_DLL_FILE_MAP value\n" );
        return 1;
    }
    if (setenv( "WINE_DLL_FILE_MAP", env_value, 1 ) == -1)
    {
        perror( "setenv(WINE_DLL_FILE_MAP)" );
        return 1;
    }

    if (argc > 4 && !strcmp( argv[4], "--" ))
    {
        child_argc = argc - 5;
        child_argv = calloc( child_argc + 2, sizeof(*child_argv) );
        if (!child_argv)
        {
            fprintf( stderr, "failed to allocate argv\n" );
            return 1;
        }

        child_argv[0] = argv[1];
        if (!child_argc)
        {
            child_argv[1] = (char *)wine_target;
        }
        else
        {
            for (i = 0; i < child_argc; i++) child_argv[i + 1] = argv[i + 5];
        }
    }
    else if (argc == 4)
    {
        child_argv = calloc( 3, sizeof(*child_argv) );
        if (!child_argv)
        {
            fprintf( stderr, "failed to allocate argv\n" );
            return 1;
        }
        child_argv[0] = argv[1];
        child_argv[1] = (char *)wine_target;
    }
    else
    {
        child_argc = argc - 4;
        child_argv = calloc( child_argc + 2, sizeof(*child_argv) );
        if (!child_argv)
        {
            fprintf( stderr, "failed to allocate argv\n" );
            return 1;
        }

        child_argv[0] = argv[1];
        for (i = 0; i < child_argc; i++) child_argv[i + 1] = argv[i + 4];
    }

    pid = fork();
    if (pid == -1)
    {
        perror( "fork" );
        return 1;
    }
    if (!pid)
    {
        execv( wine_binary, child_argv );
        perror( "execv(wine-binary)" );
        _exit( 127 );
    }

    free( env_value );
    status = wait_for_child( pid );
    cleanup_backing( &backing );
    if (status == -1) return 1;
    if (WIFEXITED( status )) return WEXITSTATUS( status );
    if (WIFSIGNALED( status )) return 128 + WTERMSIG( status );
    return 1;
}
