// Application-side sample source for LinkStatsWindow. Kept in its own
// translation unit so linkstatswindow_tests can compile LinkStatsWindow.cpp
// without LinkManager, VehicleTargetManager or MAVLinkProtocol.

#include "LinkStatsWindow.h"

#include "LinkInterface.h"
#include "LinkManager.h"
#include "MAVLinkProtocol.h"
#include "comm/VehicleTargetManager.h"

LinkStatsWindow::SampleSource LinkStatsWindow::ApplicationSampleSource()
{
    return []() {
        LinkStatsSample sample;
        LinkManager *links = LinkManager::instance();
        if (!links) {
            return sample;
        }
        VehicleTargetManager *targets = links->vehicleTargetManager();
        if (!targets) {
            return sample;
        }
        // The exact current target decides the physical link; duplicate sysids
        // on other links never contribute. Re-resolved on every tick, nothing
        // from a previous tick survives a link removal.
        const VehicleTargetLease lease = targets->acquireTarget();
        if (!lease.isValid()) {
            return sample;
        }
        const int linkId = lease.endpoint.linkId;
        LinkInterface *link = links->getLink(linkId);
        if (!link) {
            return sample;
        }
        sample.hasLink = true;
        sample.connected = link->isConnected();
        sample.linkName = link->getShortName();
        sample.rxBitsPerSecond = link->getCurrentInDataRate();   // bits/s
        sample.txBitsPerSecond = link->getCurrentOutDataRate();  // bits/s
        if (MAVLinkProtocol *protocol = links->getProtocol()) {
            sample.packetsReceived = protocol->getTotalMessagesReceived(linkId);
            sample.packetsLost = protocol->getTotalMessagesLost(linkId);
        }
        return sample;
    };
}

LinkStatsWindow *LinkStatsWindow::OpenWindow(QWidget *owner)
{
    return OpenWindow(owner, ApplicationSampleSource());
}
