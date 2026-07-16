#pragma once

namespace grandia_ap {

class ItemTracker {
public:
    void OnLocationChecked(int item_slot_id, const char* context);
    void OnChestEventChecked(unsigned event_id, const char* context);
    void OnItemAcquired(int item_slot_id, const char* context);
    void EnqueueReceivedItem(unsigned ap_item_id, const char* item_name);

private:
    bool WasCheckSent(unsigned location_id) const;
    void MarkCheckSent(unsigned location_id);
};

ItemTracker& GetItemTracker();

void SetApDelivering(bool delivering);
bool IsApDelivering();

}  // namespace grandia_ap
