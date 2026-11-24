// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Runs a command through posix_spawnp, capturing its output with a time limit (see evo_process.hpp).
#include "evo_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#include "evo_log.hpp"
extern char** environ;

namespace evo {
bool runCommand(const std::string& executable, const std::vector<std::string>& args, std::string& output, int timeoutMs,
                CommandReport* diagnostic, char* const* environment) {
    output.clear();
    if (diagnostic) {
        *diagnostic = CommandReport();
    }
    if (executable.empty() || timeoutMs <= 0) {
        if (diagnostic) {
            diagnostic->spawnError = EINVAL;
        }
        return false;
    }
    int fds[2];
    // Close-on-exec from the start: three threads run commands, and a child started by another one between pipe()
    // and fcntl() would keep this pipe's write end open (late EOF, then a timeout reported as a failure).
    if (pipe2(fds, O_CLOEXEC) != 0) {
        if (diagnostic) {
            diagnostic->spawnError = errno;
        }
        return false;
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
        if (diagnostic) {
            diagnostic->spawnError = errno;
        }
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (size_t i = 0; i < args.size(); ++i) {
        argv.push_back(const_cast<char*>(args[i].c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int initError = posix_spawn_file_actions_init(&actions);
    if (initError != 0) {
        if (diagnostic) {
            diagnostic->spawnError = initError;
        }
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    bool configured = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO) == 0 &&
                      (!diagnostic || posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO) == 0) &&
                      posix_spawn_file_actions_addclose(&actions, fds[0]) == 0 &&
                      posix_spawn_file_actions_addclose(&actions, fds[1]) == 0;
    pid_t pid = -1;
    int rc = configured ? posix_spawnp(&pid, executable.c_str(), &actions, nullptr, &argv[0],
                                       environment ? environment : environ)
                        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        if (diagnostic) {
            diagnostic->spawnError = rc;
        }
        close(fds[0]);
        return false;
    }
    bool ended = false, eof = false, ok = true;
    uint32_t begin = monoMs();
    int status = 0;
    while ((!ended || !eof) && monoMs() - begin < static_cast<uint32_t>(timeoutMs) && ok) {
        pollfd reader = {fds[0], POLLIN, 0};
        if (!eof) {
            poll(&reader, 1, 10);
        } else {
            usleep(10000);
        }
        char buf[1024];
        ssize_t n = eof ? 0 : read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            output.append(buf, static_cast<size_t>(n));
            if (output.size() > 65536) {
                output.resize(65536);
                ok = false;
                if (diagnostic) {
                    diagnostic->truncated = true;
                }
            }
        } else if (!n) {
            eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            ok = false;
        }
        if (!ended) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                ended = true;
            } else if (w < 0 && errno != EINTR) {
                ended = true;
                ok = false;
            }
        }
    }
    close(fds[0]);
    if (diagnostic) {
        diagnostic->text = output;
        diagnostic->timedOut = (!ended || !eof) && monoMs() - begin >= static_cast<uint32_t>(timeoutMs);
        if (ended && WIFEXITED(status)) {
            diagnostic->exitCode = WEXITSTATUS(status);
        }
        if (ended && WIFSIGNALED(status)) {
            diagnostic->signal = WTERMSIG(status);
        }
    }
    if (!ended) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        ok = false;
    }
    ok = ok && eof && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        output.clear();  // Never interpret a truncated/failed read as a valid scene or AP config.
    }
    return ok;
}
}  // namespace evo
