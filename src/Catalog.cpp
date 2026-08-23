///----------------------------------------------------------------------------------------------------
/// Catalog.cpp: state model and texture pipeline.
///
/// Everything the website sends lands here, and everything the UI draws is read from here.
/// Render thread only. Server owns the networking.
///----------------------------------------------------------------------------------------------------
#include "Catalog.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Addon.h"
#include "Server.h"
#include "Settings.h"
#include "Util/Util.h"

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// Constants
    ///------------------------------------------------------------------------------------------------

    /* A list that never receives its `synced` gives up after this long and offers Retry. */
    constexpr uint64_t LOADING_TIMEOUT_MS = 20000;

    /* A toggle the website never confirms clears itself after this long, so a silent website
       cannot leave a row spinning forever. */
    constexpr uint64_t PENDING_TIMEOUT_MS = 20000;

    /* A re-subscribe starts with every previous image still cached, so the "all images cached but
       no synced" escape hatch has to wait this long or it fires before the new stream starts. */
    constexpr uint64_t LOADING_ESCAPE_MIN_MS = 1000;

    /* Nexus may not have a device ready the instant we hand it bytes; re-issue the create call. */
    constexpr uint64_t TEXTURE_RETRY_MS   = 400;
    constexpr int      TEXTURE_MAX_TRIES  = 12;

    ///------------------------------------------------------------------------------------------------
    /// Types
    ///------------------------------------------------------------------------------------------------

    struct ImageSlot
    {
        Texture_t*           Texture = nullptr;
        std::string          Identifier;       /* the identifier Nexus knows this image by */
        uint64_t             Hash    = 0;      /* of the encoded bytes, to skip identical re-streams */
        std::vector<uint8_t> Bytes;            /* retained only until the texture resolves */
        uint64_t             NextTryMs = 0;
        int                  Tries     = 0;
    };

    struct ListImages
    {
        std::map<int, ImageSlot> Rows;
        std::map<int, ImageSlot> Hovers;
    };

    struct LoadInfo
    {
        Catalog::LoadState State   = Catalog::LoadState::Idle;
        uint64_t           StartMs = 0;
    };

    ///------------------------------------------------------------------------------------------------
    /// State
    ///------------------------------------------------------------------------------------------------

    std::vector<Protocol::List>       s_lists;
    std::map<std::string, ListImages> s_images;

    /* The open set IS the subscription set. s_subscribed is the last set actually sent. */
    std::vector<std::string>          s_openLists;
    std::set<std::string>             s_subscribed;

    std::map<std::string, LoadInfo>                s_load;
    std::map<std::string, std::map<int, uint64_t>> s_pending;   /* listId -> index -> sent at */

    bool s_connected = false;
    bool s_restored  = false;   /* persisted lists restored for this connection */

    uint64_t s_texturesMade = 0;

    ///------------------------------------------------------------------------------------------------
    /// Small helpers
    ///------------------------------------------------------------------------------------------------

    uint64_t Now()
    {
        return (uint64_t)GetTickCount64();
    }

    /* These are unsigned milliseconds, so a start stamp even one tick newer than aNow would wrap
       the subtraction to ~2^64 and fire every timeout at once. Pump() reads its clock before
       applying messages that stamp fresh times, so that ordering really does happen. */
    uint64_t Since(uint64_t aNow, uint64_t aStart)
    {
        return (aNow > aStart) ? (aNow - aStart) : 0;
    }

    uint64_t HashBytes(const std::vector<uint8_t>& aBytes)
    {
        /* FNV-1a. Only ever compared against another hash of the same slot. */
        uint64_t h = 1469598103934665603ULL;
        for (uint8_t b : aBytes)
        {
            h ^= (uint64_t)b;
            h *= 1099511628211ULL;
        }
        return h;
    }

    Protocol::List* FindMutable(const std::string& aListId)
    {
        for (Protocol::List& list : s_lists)
        {
            if (list.Id == aListId) { return &list; }
        }
        return nullptr;
    }

    bool IsOpenId(const std::string& aListId)
    {
        return std::find(s_openLists.begin(), s_openLists.end(), aListId) != s_openLists.end();
    }

    int EntryCountOf(const std::string& aListId)
    {
        const Protocol::List* list = Catalog::Find(aListId);
        return list != nullptr ? (int)list->Entries.size() : 0;
    }

    /* Counts the images we hold, whether or not the GPU upload has landed. */
    int ImagesHeld(const std::string& aListId)
    {
        auto it = s_images.find(aListId);
        if (it == s_images.end()) { return 0; }

        int count = 0;
        for (const auto& kv : it->second.Rows)
        {
            if (!kv.second.Identifier.empty()) { count++; }
        }
        return count;
    }

    ///------------------------------------------------------------------------------------------------
    /// Textures
    ///
    /// An identifier is the FNV-1a hash of the encoded bytes. Nexus keys its texture cache by
    /// identifier and ignores the bytes for one it already knows, so identifier and image have to
    /// stay 1:1 or it hands back the wrong picture.
    ///
    /// TODO(texture-lifetime): Nexus API v6 has no texture release call, so entries live for the
    /// whole game session. Hashing bounds that to the number of distinct images shown, since a
    /// re-stream, a reconnect or an addon reload all reuse what is already there. A timer list
    /// that re-renders every minute still grows over hours, and that fix has to come from Nexus.
    ///------------------------------------------------------------------------------------------------

    bool CreateTexture(ImageSlot& aSlot, uint64_t aNow)
    {
        if (APIDefs == nullptr || APIDefs->Textures_GetOrCreateFromMemory == nullptr) { return false; }
        if (aSlot.Bytes.empty()) { return false; }

        aSlot.Tries++;
        aSlot.NextTryMs = aNow + TEXTURE_RETRY_MS;

        Texture_t* tex = APIDefs->Textures_GetOrCreateFromMemory(
            aSlot.Identifier.c_str(), (void*)aSlot.Bytes.data(), (uint64_t)aSlot.Bytes.size());

        /* A null pointer (or a texture whose resource has not been uploaded yet) means retry,
           never a cached failure. */
        if (tex == nullptr || tex->Resource == nullptr) { return false; }

        aSlot.Texture = tex;
        aSlot.Bytes.clear();
        aSlot.Bytes.shrink_to_fit();

        s_texturesMade++;
        if ((s_texturesMade % 250) == 0)
        {
            Addon::Log(LOGL_DEBUG, "Textures created since load: %llu (identifiers are content "
                                   "addressed; Nexus v6 has no release call).",
                       (unsigned long long)s_texturesMade);
        }
        return true;
    }

    /* Decodes and uploads, replacing whatever the slot held. Returns false on a malformed payload. */
    bool StoreImage(ImageSlot& aSlot, const std::string& aPrefix, const std::string& aListId,
                    int aIndex, const std::string& aB64)
    {
        std::vector<uint8_t> bytes;
        if (!Util::Base64Decode(aB64, bytes) || bytes.empty()) { return false; }

        uint64_t hash = HashBytes(bytes);
        /* Byte-identical re-stream: keep the texture we already have. The Texture check is not
           redundant. Without it a slot whose upload never landed would match on hash and be
           skipped forever, leaving that row blank. */
        if (hash == aSlot.Hash && !aSlot.Identifier.empty() && aSlot.Texture != nullptr)
        {
            Addon::Log(LOGL_TRACE, "Identical re-stream for %s %s[%d]; keeping existing texture.",
                       aPrefix.c_str(), aListId.c_str(), aIndex);
            return true;
        }

        /* This replaced a monotonic counter, which restarts at 0 on every addon load while Nexus'
           texture registry survives the reload. After a hot reload we handed Nexus identifiers it
           had seen last session and it gave us back those old images. */
        char id[64];
        snprintf(id, sizeof(id), "GW2APP_IMG_%016llx", (unsigned long long)hash);

        /* Leave aSlot.Texture on the old image until the new one has uploaded. Nexus queues the
           upload, so clearing it here made the row vanish for a retry interval and then pop back,
           which showed on every checkbox tick. */
        aSlot.Identifier = id;
        aSlot.Hash       = hash;
        aSlot.Bytes      = std::move(bytes);
        aSlot.NextTryMs  = 0;
        aSlot.Tries      = 0;

        CreateTexture(aSlot, Now());
        return true;
    }

    /* Textures that were not ready when their bytes arrived get another go, cheaply. */
    void ResolvePendingTextures(uint64_t aNow)
    {
        if (APIDefs == nullptr) { return; }

        for (auto& listKv : s_images)
        {
            for (auto* bucket : { &listKv.second.Rows, &listKv.second.Hovers })
            {
                for (auto& kv : *bucket)
                {
                    ImageSlot& slot = kv.second;
                    /* Bytes still held means an upload is pending. A slot can hold a live older
                       texture at the same time, so a non-null Texture is no reason to stop. */
                    if (slot.Bytes.empty()) { continue; }
                    if (aNow < slot.NextTryMs) { continue; }

                    if (slot.Tries >= TEXTURE_MAX_TRIES)
                    {
                        /* Give the memory back. If the slot still holds an older texture the row
                           keeps showing that. A stale row beats a missing one. */
                        Addon::Log(LOGL_WARNING, "Gave up uploading texture %s after %d attempts.",
                                   slot.Identifier.c_str(), slot.Tries);
                        slot.Bytes.clear();
                        slot.Bytes.shrink_to_fit();
                        continue;
                    }

                    CreateTexture(slot, aNow);
                }
            }
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// Load state
    ///------------------------------------------------------------------------------------------------

    void MarkLoading(const std::string& aListId)
    {
        LoadInfo& info = s_load[aListId];
        if (info.State != Catalog::LoadState::Loading)
        {
            info.StartMs = Now();
        }
        info.State = Catalog::LoadState::Loading;   /* also clears a previous failure */
    }

    void MarkLoaded(const std::string& aListId)
    {
        s_load.erase(aListId);
    }

    ///------------------------------------------------------------------------------------------------
    /// Subscriptions
    ///------------------------------------------------------------------------------------------------

    void SendSubscribe(const std::vector<std::string>& aIds)
    {
        Server::Send(Protocol::BuildSubscribe(aIds, Settings::RenderWidth()));
    }

    /* The open set is the subscription set. */
    void SyncSubscriptions()
    {
        if (!s_connected) { return; }

        std::set<std::string> next(s_openLists.begin(), s_openLists.end());
        if (next == s_subscribed) { return; }

        for (const std::string& id : next)
        {
            if (s_subscribed.count(id) == 0) { MarkLoading(id); }
        }
        for (const std::string& id : s_subscribed)
        {
            if (next.count(id) == 0) { MarkLoaded(id); }
        }

        s_subscribed = next;
        SendSubscribe(s_openLists);
    }

    void AddOpen(const std::string& aListId)
    {
        if (aListId.empty() || IsOpenId(aListId)) { return; }
        s_openLists.push_back(aListId);
    }

    void RemoveOpen(const std::string& aListId)
    {
        auto it = std::find(s_openLists.begin(), s_openLists.end(), aListId);
        if (it != s_openLists.end()) { s_openLists.erase(it); }
    }

    /* Call this only when the user opened or closed the list. A programmatic close (the list
       vanished, or we disconnected) must never unpersist, or a blip on the website would cost
       the user their windows. */
    void Persist(const std::string& aListId, bool aOpen)
    {
        std::vector<std::string> set = Settings::OpenLists();
        auto it = std::find(set.begin(), set.end(), aListId);

        if (aOpen)
        {
            if (it != set.end()) { return; }
            set.push_back(aListId);
        }
        else
        {
            if (it == set.end()) { return; }
            set.erase(it);
        }

        Settings::SetOpenLists(set);
    }

    ///------------------------------------------------------------------------------------------------
    /// Caches
    ///------------------------------------------------------------------------------------------------

    void DropListImages(const std::string& aListId)
    {
        s_images.erase(aListId);
    }

    void DropEntryCaches()
    {
        s_images.clear();
        s_pending.clear();
    }

    void ClearPending(const std::string& aListId, int aIndex)
    {
        auto it = s_pending.find(aListId);
        if (it == s_pending.end()) { return; }

        it->second.erase(aIndex);
        if (it->second.empty()) { s_pending.erase(it); }
    }

    /* A new row image is the website's signal that the row's data changed, so any hover card we
       cached for it is now stale. */
    void InvalidateHover(const std::string& aListId, int aIndex)
    {
        auto it = s_images.find(aListId);
        if (it == s_images.end()) { return; }
        it->second.Hovers.erase(aIndex);
    }

    ///------------------------------------------------------------------------------------------------
    /// Message application
    ///------------------------------------------------------------------------------------------------

    /* Only a new list, a changed entry count, or a moved completed/autoCompleted flag counts.
       A rename leaves load state alone, so retitling a list does not blank its window. */
    bool EntriesChanged(const std::vector<Protocol::List>& aOld, const Protocol::List& aNew)
    {
        const Protocol::List* old = nullptr;
        for (const Protocol::List& list : aOld)
        {
            if (list.Id == aNew.Id) { old = &list; break; }
        }

        if (old == nullptr) { return true; }
        if (old->Entries.size() != aNew.Entries.size()) { return true; }

        for (size_t i = 0; i < aNew.Entries.size(); i++)
        {
            if (old->Entries[i].Completed     != aNew.Entries[i].Completed ||
                old->Entries[i].AutoCompleted != aNew.Entries[i].AutoCompleted)
            {
                return true;
            }
        }
        return false;
    }

    void RestorePersistedOpenLists()
    {
        int opened = 0;
        for (const std::string& id : Settings::OpenLists())
        {
            if (id.empty() || IsOpenId(id))     { continue; }
            if (Catalog::Find(id) == nullptr)   { continue; }   /* skipped, never pruned */

            AddOpen(id);
            opened++;
        }

        Addon::Log(LOGL_INFO, "Restore: reopened %d list window(s) of %d persisted.",
                   opened, (int)Settings::OpenLists().size());
    }

    void ApplyState(Protocol::StateMsg& aMsg)
    {
        std::vector<Protocol::List> old;
        old.swap(s_lists);
        s_lists = std::move(aMsg.Lists);

        /* Imagery is pruned on `synced`, not here, so every row keeps its previous image until a
           fresh one lands. */

        /* Carry chat links and custom-entry URLs across by index. A `state` payload does not
           include them (they arrive only on `entry` messages) and we store them on the entry
           itself, so without this a `state` would wipe every link and quietly break "Copy
           waypoints" and row click-to-copy until something re-streamed. */
        for (Protocol::List& list : s_lists)
        {
            if (list.Id.empty()) { continue; }

            const Protocol::List* prev = nullptr;
            for (const Protocol::List& candidate : old)
            {
                if (candidate.Id == list.Id) { prev = &candidate; break; }
            }
            if (prev == nullptr) { continue; }

            const size_t shared = std::min(list.Entries.size(), prev->Entries.size());
            for (size_t i = 0; i < shared; ++i)
            {
                if (list.Entries[i].ChatLink.empty()) { list.Entries[i].ChatLink = prev->Entries[i].ChatLink; }
                if (list.Entries[i].Link.empty())     { list.Entries[i].Link     = prev->Entries[i].Link; }
            }
        }

        /* First `state` of this connection: bring the user's windows back. */
        if (!s_restored)
        {
            s_restored = true;
            RestorePersistedOpenLists();
        }

        for (const Protocol::List& list : s_lists)
        {
            if (list.Id.empty()) { continue; }
            if (s_subscribed.count(list.Id) != 0 && EntriesChanged(old, list))
            {
                MarkLoading(list.Id);
            }
        }

        /* A list deleted on the website closes its window and drops its imagery. This is a
           programmatic close, so the persisted set keeps the id and it comes back if the list does. */
        std::vector<std::string> gone;
        for (const std::string& id : s_openLists)
        {
            if (Catalog::Find(id) == nullptr) { gone.push_back(id); }
        }
        for (const std::string& id : gone)
        {
            RemoveOpen(id);
            DropListImages(id);
            MarkLoaded(id);
        }

        SyncSubscriptions();
    }

    void ApplyEntry(const Protocol::EntryMsg& aMsg)
    {
        if (aMsg.ListId.empty() || aMsg.Index < 0) { return; }

        /* Log rather than drop in silence: an entry for a list we do not know, or an index past
           the end of what we hold, means our catalog and the website's have diverged. */
        Protocol::List* list = FindMutable(aMsg.ListId);
        if (list == nullptr)
        {
            Addon::Log(LOGL_WARNING, "Dropped entry for unknown list %s[%d].",
                       aMsg.ListId.c_str(), aMsg.Index);
            return;
        }
        if (aMsg.Index >= (int)list->Entries.size())
        {
            Addon::Log(LOGL_WARNING, "Dropped entry %s[%d]: our state holds only %d entries.",
                       aMsg.ListId.c_str(), aMsg.Index, (int)list->Entries.size());
            return;
        }

        Protocol::Entry& entry = list->Entries[aMsg.Index];

        /* Never wipe the last known name with an empty payload. */
        if (!aMsg.Name.empty()) { entry.Name = aMsg.Name; }

        entry.ChatLink      = aMsg.ChatLink;
        entry.Link          = aMsg.Link;
        entry.Completed     = aMsg.Completed;
        entry.AutoCompleted = aMsg.AutoCompleted;
        entry.HasHoverCard  = aMsg.HasHoverCard;

        if (!aMsg.ImageB64.has_value() || aMsg.ImageB64->empty()) { return; }

        ImageSlot& slot = s_images[aMsg.ListId].Rows[aMsg.Index];
        if (!StoreImage(slot, "E", aMsg.ListId, aMsg.Index, *aMsg.ImageB64))
        {
            /* Keep the previous image; a failed decode must not blank a row. */
            Addon::Log(LOGL_WARNING, "Failed to decode image for %s[%d] (%s).",
                       aMsg.ListId.c_str(), aMsg.Index, aMsg.Mime.c_str());
            return;
        }

        /* The new image is what settles a user-toggled checkbox, so the spinner clears in the
           same frame as the new pixels. */
        ClearPending(aMsg.ListId, aMsg.Index);
        InvalidateHover(aMsg.ListId, aMsg.Index);
    }

    void ApplyHoverImage(const Protocol::HoverImgMsg& aMsg)
    {
        if (aMsg.ListId.empty() || aMsg.Index < 0 || aMsg.ImageB64.empty()) { return; }

        ImageSlot& slot = s_images[aMsg.ListId].Hovers[aMsg.Index];
        if (!StoreImage(slot, "H", aMsg.ListId, aMsg.Index, aMsg.ImageB64))
        {
            Addon::Log(LOGL_WARNING, "Failed to decode hover image for %s[%d] (%s).",
                       aMsg.ListId.c_str(), aMsg.Index, aMsg.Mime.c_str());
        }
    }

    void ApplySynced(const Protocol::SyncedMsg& aMsg)
    {
        for (const std::string& id : aMsg.ListIds)
        {
            if (id.empty()) { continue; }

            /* Entries were removed on the website: drop imagery that is now out of range.
               A list that left the catalog entirely resolves to 0 and loses everything. */
            int valid = EntryCountOf(id);

            auto it = s_images.find(id);
            if (it != s_images.end())
            {
                for (auto* bucket : { &it->second.Rows, &it->second.Hovers })
                {
                    for (auto slotIt = bucket->begin(); slotIt != bucket->end(); )
                    {
                        slotIt = (slotIt->first >= valid) ? bucket->erase(slotIt) : std::next(slotIt);
                    }
                }
            }

            bool wasLoading = (s_load.count(id) != 0);
            MarkLoaded(id);
            Addon::Log(LOGL_DEBUG, "Synced: cleared loading=%s for %s.",
                       wasLoading ? "true" : "false", id.c_str());
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// Connection events
    ///------------------------------------------------------------------------------------------------

    /* A new client took over. The catalog KEEPS its lists so the windows stay on screen until the
       new client's first `state` lands. Only the per-connection bookkeeping is flushed. */
    void OnClientReplaced()
    {
        s_pending.clear();
        s_load.clear();
        s_subscribed.clear();
        s_restored = false;

        /* Row images are kept: they are replaced per row as the new client streams, which avoids a
           blank window and a full re-decode. Hover cards are dropped because they go stale
           invisibly. */
        for (auto& kv : s_images)
        {
            kv.second.Hovers.clear();
        }

        Addon::Log(LOGL_INFO, "Client replaced; keeping catalog, awaiting a fresh state.");
    }

    ///------------------------------------------------------------------------------------------------
    /// Per-frame sweeps
    ///------------------------------------------------------------------------------------------------

    void SweepLoading(uint64_t aNow)
    {
        for (auto& kv : s_load)
        {
            if (kv.second.State != Catalog::LoadState::Loading) { continue; }

            uint64_t elapsed = Since(aNow, kv.second.StartMs);

            /* Escape hatch: every image is here but `synced` never came, so clear loading
               ourselves rather than spin forever. */
            if (elapsed >= LOADING_ESCAPE_MIN_MS)
            {
                int total = EntryCountOf(kv.first);
                if (ImagesHeld(kv.first) >= total)
                {
                    Addon::Log(LOGL_INFO, "All %d images cached for list %s but no 'synced' received; "
                                          "auto-clearing loading state.", total, kv.first.c_str());
                    kv.second.State = Catalog::LoadState::Idle;
                    continue;
                }
            }

            if (elapsed > LOADING_TIMEOUT_MS)
            {
                Addon::Log(LOGL_WARNING, "Loading timeout for list %s after %ds; surfacing Retry.",
                           kv.first.c_str(), (int)(LOADING_TIMEOUT_MS / 1000));
                kv.second.State = Catalog::LoadState::Failed;
            }
        }

        for (auto it = s_load.begin(); it != s_load.end(); )
        {
            it = (it->second.State == Catalog::LoadState::Idle) ? s_load.erase(it) : std::next(it);
        }
    }

    /* A toggle the website never confirms is cleared here, so a row cannot stay darkened forever. */
    void SweepPending(uint64_t aNow)
    {
        for (auto listIt = s_pending.begin(); listIt != s_pending.end(); )
        {
            for (auto it = listIt->second.begin(); it != listIt->second.end(); )
            {
                if (Since(aNow, it->second) > PENDING_TIMEOUT_MS)
                {
                    Addon::Log(LOGL_WARNING, "No confirmation for %s[%d] after %ds; clearing pending.",
                               listIt->first.c_str(), it->first, (int)(PENDING_TIMEOUT_MS / 1000));
                    it = listIt->second.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            listIt = listIt->second.empty() ? s_pending.erase(listIt) : std::next(listIt);
        }
    }
}

namespace Catalog
{
    ///------------------------------------------------------------------------------------------------
    /// Pump
    ///------------------------------------------------------------------------------------------------

    void Pump()
    {
        for (Server::Event ev : Server::TakeEvents())
        {
            switch (ev)
            {
                case Server::Event::Connected:
                    s_connected = true;
                    s_subscribed.clear();
                    s_restored = false;
                    Addon::Log(LOGL_INFO, "Client connected.");
                    break;

                /* A lost connection drops everything; a replaced client keeps the catalog. */
                case Server::Event::ConnectionLost:
                    Addon::Log(LOGL_INFO, "Connection lost; dropping catalog and closing lists.");
                    Reset();
                    break;

                case Server::Event::ClientReplaced:
                    OnClientReplaced();
                    break;
            }
        }

        for (const std::string& json : Server::TakeInbound())
        {
            Protocol::Inbound msg = Protocol::Parse(json);
            switch (msg.Type)
            {
                case Protocol::Kind::State:      ApplyState(msg.State);            break;
                case Protocol::Kind::Entry:      ApplyEntry(msg.EntryData);        break;
                case Protocol::Kind::Synced:     ApplySynced(msg.Synced);          break;
                case Protocol::Kind::HoverImage: ApplyHoverImage(msg.HoverImage);  break;
                case Protocol::Kind::Unknown:                                      break;
            }
        }

        /* Re-sample: applying the messages above can take milliseconds (base64 and texture upload)
           and stamps timers with a fresh clock, so the sweeps must not run against a reading taken
           before that work. */
        const uint64_t swept = Now();
        ResolvePendingTextures(swept);
        SweepLoading(swept);
        SweepPending(swept);
        SyncSubscriptions();
    }

    void Reset()
    {
        s_lists.clear();
        s_openLists.clear();
        s_subscribed.clear();
        s_load.clear();
        DropEntryCaches();

        s_connected = false;
        s_restored  = false;
    }

    ///------------------------------------------------------------------------------------------------
    /// Lists
    ///------------------------------------------------------------------------------------------------

    const std::vector<Protocol::List>& Lists()
    {
        return s_lists;
    }

    const Protocol::List* Find(const std::string& aListId)
    {
        if (aListId.empty()) { return nullptr; }

        for (const Protocol::List& list : s_lists)
        {
            if (list.Id == aListId) { return &list; }
        }
        return nullptr;
    }

    bool IsConnected()
    {
        return s_connected;
    }

    ///------------------------------------------------------------------------------------------------
    /// Open windows / subscriptions
    ///------------------------------------------------------------------------------------------------

    void OpenList(const std::string& aListId)
    {
        if (aListId.empty() || Find(aListId) == nullptr) { return; }
        if (IsOpenId(aListId)) { return; }

        AddOpen(aListId);
        Persist(aListId, true);
        SyncSubscriptions();
    }

    void CloseList(const std::string& aListId)
    {
        if (!IsOpenId(aListId)) { return; }

        RemoveOpen(aListId);
        Persist(aListId, false);   /* the user said "not this list", so forget it */
        MarkLoaded(aListId);
        SyncSubscriptions();
    }

    bool IsOpen(const std::string& aListId)
    {
        return IsOpenId(aListId);
    }

    const std::vector<std::string>& OpenLists()
    {
        return s_openLists;
    }

    void RetryList(const std::string& aListId)
    {
        if (!IsOpenId(aListId)) { return; }

        /* Drop everything we hold for the list so the incoming stream replaces cleanly. */
        DropListImages(aListId);
        s_pending.erase(aListId);
        MarkLoading(aListId);

        /* Subscribe without the id, then with it. The website treats that as a fresh
           subscription and re-streams every image followed by `synced`. No wire change. */
        std::vector<std::string> without;
        for (const std::string& id : s_openLists)
        {
            if (id != aListId) { without.push_back(id); }
        }

        SendSubscribe(without);
        SendSubscribe(s_openLists);

        s_subscribed = std::set<std::string>(s_openLists.begin(), s_openLists.end());
        Addon::Log(LOGL_INFO, "Retry: re-subscribed %s.", aListId.c_str());
    }

    void ResendSubscribe()
    {
        if (!s_connected || s_openLists.empty()) { return; }

        /* Leaves load state alone, so the textures we have stay on screen (slightly soft) until
           crisp ones arrive. The user only nudged the scale slider; a spinner would be worse. */
        SendSubscribe(s_openLists);
        s_subscribed = std::set<std::string>(s_openLists.begin(), s_openLists.end());
    }

    ///------------------------------------------------------------------------------------------------
    /// Load state
    ///------------------------------------------------------------------------------------------------

    LoadState StateOf(const std::string& aListId)
    {
        auto it = s_load.find(aListId);
        return it != s_load.end() ? it->second.State : LoadState::Idle;
    }

    void LoadProgress(const std::string& aListId, int& aOutHave, int& aOutTotal)
    {
        aOutTotal = EntryCountOf(aListId);
        aOutHave  = ImagesHeld(aListId);

        if (aOutHave > aOutTotal) { aOutHave = aOutTotal; }
    }

    ///------------------------------------------------------------------------------------------------
    /// Entry images and pending toggles
    ///------------------------------------------------------------------------------------------------

    Texture_t* EntryImage(const std::string& aListId, int aIndex)
    {
        auto listIt = s_images.find(aListId);
        if (listIt == s_images.end()) { return nullptr; }

        auto it = listIt->second.Rows.find(aIndex);
        return it != listIt->second.Rows.end() ? it->second.Texture : nullptr;
    }

    Texture_t* HoverImage(const std::string& aListId, int aIndex)
    {
        auto listIt = s_images.find(aListId);
        if (listIt == s_images.end()) { return nullptr; }

        auto it = listIt->second.Hovers.find(aIndex);
        return it != listIt->second.Hovers.end() ? it->second.Texture : nullptr;
    }

    void SetCompleted(const std::string& aListId, int aIndex, bool aCompleted)
    {
        const Protocol::List* list = Find(aListId);
        if (list == nullptr || aIndex < 0 || aIndex >= (int)list->Entries.size()) { return; }

        /* The protocol forbids both of these. The UI already prevents them; this is the backstop. */
        if (list->IsLootBag)
        {
            Addon::Log(LOGL_WARNING, "Refusing set_entry_completed on loot bag list %s.", aListId.c_str());
            return;
        }
        if (list->Entries[aIndex].AutoCompleted)
        {
            Addon::Log(LOGL_WARNING, "Refusing set_entry_completed on auto-completed %s[%d].",
                       aListId.c_str(), aIndex);
            return;
        }

        Server::Send(Protocol::BuildSetEntryCompleted(aListId, aIndex, aCompleted));

        /* The website owns completion state, so leave the flag as it last reported it. The row is
           marked pending and settles when a new image for it arrives. */
        s_pending[aListId][aIndex] = Now();
    }

    bool IsPending(const std::string& aListId, int aIndex)
    {
        auto it = s_pending.find(aListId);
        if (it == s_pending.end()) { return false; }

        return it->second.count(aIndex) != 0;
    }

    ///------------------------------------------------------------------------------------------------
    /// Hover streaming
    ///------------------------------------------------------------------------------------------------

    void OpenHover(const std::string& aListId, int aIndex)
    {
        const Protocol::List* list = Find(aListId);
        if (list == nullptr || aIndex < 0 || aIndex >= (int)list->Entries.size()) { return; }
        if (!list->Entries[aIndex].HasHoverCard) { return; }

        /* Checks the live socket, not the UI connection flag: with no client the frames can never
           arrive and the spinner would stick to the cursor. */
        if (!Server::IsClientConnected()) { return; }

        Server::Send(Protocol::BuildOpenHover(aListId, aIndex));
    }

    void CloseHover(const std::string& aListId, int aIndex)
    {
        if (aListId.empty() || aIndex < 0) { return; }

        /* Sent even when the card is being torn down, otherwise the website is left streaming a
           hover card nobody is showing. */
        Server::Send(Protocol::BuildCloseHover(aListId, aIndex));
    }
}
