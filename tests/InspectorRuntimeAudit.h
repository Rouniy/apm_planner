#ifndef INSPECTORRUNTIMEAUDIT_H
#define INSPECTORRUNTIMEAUDIT_H

// Exercises the production pre-heartbeat MAVLink Inspector ingress path.
// Returns zero on success and one if exact-link or parser lifecycle behavior
// regresses.
int RunInspectorRuntimeAudit();

#endif // INSPECTORRUNTIMEAUDIT_H
