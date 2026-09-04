#ifndef SETUPROUTERUNTIMEAUDIT_H
#define SETUPROUTERUNTIMEAUDIT_H

// Runs a side-effect-free audit of the production SETUP navigation and page
// factories. Returns zero on success and a non-zero process exit code on any
// inventory, routing, ownership or semantic-content failure.
int RunSetupRouteRuntimeAudit();

#endif // SETUPROUTERUNTIMEAUDIT_H
