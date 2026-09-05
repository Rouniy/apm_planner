#ifndef LOGDOWNLOADRUNTIMEAUDIT_H
#define LOGDOWNLOADRUNTIMEAUDIT_H

// Exercises the production stamped UDP ingress and exact Download Logs route.
// Returns zero on success and one when peer/session isolation regresses.
int RunLogDownloadRuntimeAudit();

#endif // LOGDOWNLOADRUNTIMEAUDIT_H
