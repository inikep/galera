/* Copyright (C) 2011 Codership Oy <info@codership.com> */

#include "garb_config.hpp"
#include "garb_recv_loop.hpp"

#include <gu_throw.hpp>

#include <iostream>

#include <stdlib.h> // exit()
#include <unistd.h> // setsid(), chdir()
#include <fcntl.h>  // open()

namespace garb
{

void
become_daemon (const std::string& workdir)
{
    if (chdir("/")) // detach from potentially removable block devices
    {
        gu_throw_error(errno) << "chdir(" << workdir << ") failed";
    }

    if (!workdir.empty() && chdir(workdir.c_str()))
    {
        gu_throw_error(errno) << "chdir(" << workdir << ") failed";
    }

    if (pid_t pid = fork())
    {
        if (pid > 0) // parent
        {
            exit(0);
        }
        else
        {
            // I guess we want this to go to stderr as well;
            std::cerr << "Failed to fork daemon process: "
                      << errno << " (" << strerror(errno) << ")";
            gu_throw_error(errno) << "Failed to fork daemon process";
        }
    }

    // child

    if (setsid()<0) // become a new process leader, detach from terminal
    {
        gu_throw_error(errno) << "setsid() failed";
    }

    // umask(0);

    // A second fork ensures the process cannot acquire a controlling
    // terminal.
    if (pid_t pid = fork())
    {
        if (pid > 0)
        {
            exit(0);
        }
        else
        {
            gu_throw_error(errno) << "Second fork failed";
        }
    }

    // Close the standard streams. This decouples the daemon from the
    // terminal that started it.
    close(0);
    close(1);
    close(2);

    // Bind standard fds (0, 1, 2) to /dev/null
    for (int fd = 0; fd < 3; ++fd)
    {
        if (open("/dev/null", O_RDONLY) < 0)
        {
            gu_throw_error(errno) << "Unable to open /dev/null for fd " << fd;
        }
    }

    char* wd(static_cast<char*>(::malloc(PATH_MAX)));
    if (wd)
    {
        log_info << "Currend WD: " << getcwd(wd, PATH_MAX);
        ::free(wd);
    }
}

int
main (int argc, char* argv[])
{
    Config config(argc, argv);
    if (config.exit()) return 0;

    log_info << "Read config: " <<  config << std::endl;

    if (config.daemon()) become_daemon(config.workdir());

    try
    {
        RecvLoop loop (config);
        return loop.returnCode();
    }
    catch (std::exception& e)
    {
        log_fatal << "Garbd exiting with error: " << e.what();
    }
    catch (...)
    {
        log_fatal << "Garbd exiting.";
    }

    return EXIT_FAILURE;
}

} /* namespace garb */

int
main (int argc, char* argv[])
{
    try
    {
        return garb::main (argc, argv);
    }
    catch (std::exception& e)
    {
        log_fatal << e.what();
        return 1;
    }
}

