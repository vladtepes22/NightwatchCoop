/* The game's log output (stdout, unbuffered) goes into a big in-memory pipe that a background thread
 * forwards to the real stdout (the launcher's pipe, a log file). A reader that stops for a while (a busy
 * launcher, an antivirus scanning the log, a full 4 KiB launcher pipe) then stalls that thread, not
 * whichever game thread printed a log line. BB_LOG_ASYNC=0 (or bbport.ini log_async=0) keeps the direct
 * writes. Windows only; elsewhere the functions do nothing. */
#ifndef RUNTIME_LOGDRAIN_H
#define RUNTIME_LOGDRAIN_H
#ifdef __cplusplus
extern "C" {
#endif
void runtime_logdrain_start(void);
/* Waits (up to timeout_ms) until everything printed so far has been handed to the real stdout: call it
 * before the process ends without running atexit handlers (TerminateProcess, _exit). */
void runtime_logdrain_flush(unsigned timeout_ms);
#ifdef __cplusplus
}
#endif
#ifdef _WIN32
#define BB_TERMINATE_PROCESS(code) (runtime_logdrain_flush(3000), TerminateProcess(GetCurrentProcess(), (code)))
#endif
#endif
