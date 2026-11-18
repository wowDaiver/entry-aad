// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdlib.h>

#include <string>

#include "trial_guard.hpp"
static void arm(const std::string& dir, const char* value = telemetry::TrialToken) {
    int fd = open((dir + "/trial-arm").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, value, strlen(value)) == static_cast<ssize_t>(strlen(value)));
    close(fd);
}
int main() {
#ifdef __APPLE__
    char pattern[] = "/private/tmp/telemetry-guard-XXXXXX";
#else
    char pattern[] = "/tmp/telemetry-guard-XXXXXX";
#endif
    char* p = mkdtemp(pattern);
    assert(p);
    std::string dir = p;
    assert(!telemetry::consume_trial(p));
    arm(dir, "bad\n");
    assert(!telemetry::consume_trial(p));
    assert(!unlink((dir + "/trial-arm").c_str()));
    arm(dir);
    assert(telemetry::consume_trial(p));
    assert(!telemetry::consume_trial(p));
    // Even a surviving/recreated arm cannot retry after the durable claim.
    arm(dir);
    assert(!telemetry::consume_trial(p));
    assert(!unlink((dir + "/trial-arm").c_str()));
    assert(!unlink((dir + "/trial-consumed").c_str()));
    arm(dir);
    assert(!chmod((dir + "/trial-arm").c_str(), 0644));
    assert(!telemetry::consume_trial(p));
    assert(!unlink((dir + "/trial-arm").c_str()));
    assert(!symlink("/etc/passwd", (dir + "/trial-arm").c_str()));
    assert(!telemetry::consume_trial(p));
    assert(!unlink((dir + "/trial-arm").c_str()));
    assert(!chmod(p, 0777));
    arm(dir);
    assert(!telemetry::consume_trial(p));
    assert(!chmod(p, 0700));
    assert(!unlink((dir + "/trial-arm").c_str()));
    telemetry::trial_log("test", 4, "87", p);
    int log = open((dir + "/trial.log").c_str(), O_WRONLY);
    assert(log >= 0);
    assert(!ftruncate(log, 65536));
    close(log);
    telemetry::trial_log("rotated", 4, "88", p);
    struct stat st = {};
    assert(!stat((dir + "/trial.log.previous").c_str(), &st) && st.st_size == 65536);
    assert(!stat((dir + "/trial.log").c_str(), &st) && st.st_size > 0 && st.st_size < 512);
    assert(!unlink((dir + "/trial.log.previous").c_str()));
    assert(!symlink("/etc/passwd", (dir + "/trial.log.previous").c_str()));
    log = open((dir + "/trial.log").c_str(), O_WRONLY);
    assert(log >= 0);
    assert(!ftruncate(log, 65536));
    close(log);
    telemetry::trial_log("unsafe-rotation", 4, "89", p);
    assert(!lstat((dir + "/trial.log.previous").c_str(), &st) && S_ISLNK(st.st_mode));
    assert(!unlink((dir + "/trial.log.previous").c_str()));
    assert(!unlink((dir + "/trial.log").c_str()));
    assert(!rmdir(p));
    puts("trial guard: missing/bad/unsafe permit rejected; single durable claim; retries rejected");
}
