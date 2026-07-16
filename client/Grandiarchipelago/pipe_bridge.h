#pragma once

namespace grandia_ap {

void StartPipeBridge();
void StopPipeBridge();
void PipeEnqueueLocationCheck(unsigned location_id);
// Tell the bridge the save's last applied ReceivedItems count (AP Index).
// Bridge skips redelivery for items with Index <= this value.
// force=true: always announce (confirm-load). force=false: skip if same index already sent.
void PipeEnqueueSync(unsigned received_index, bool force = false);
void PipePoll();

}  // namespace grandia_ap
