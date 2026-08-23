#include "Protocol.h"

#include <limits>
#include <utility>

#include "json/json.hpp"

#include "Addon.h"

namespace Protocol
{
    namespace
    {
        using Json    = nlohmann::json;
        using OutJson = nlohmann::ordered_json;   /* emits fields in insertion order, so the shape we send is stable */

        ///--------------------------------------------------------------------------------------------
        /// Non-throwing accessors.
        ///
        /// A wrong type, a null, or a missing key has to give back a default, never an exception.
        /// nlohmann's find() works on non-objects too (it returns end()), so these are safe on
        /// any value.
        ///--------------------------------------------------------------------------------------------

        std::string StrOf(const Json& aObj, const char* aKey)
        {
            auto it = aObj.find(aKey);
            if (it == aObj.end() || !it->is_string()) { return std::string(); }
            return it->get<std::string>();
        }

        /* Same, but steals the buffer. Base64 payloads run to megabytes; aObj is ours to gut. */
        std::string TakeStrOf(Json& aObj, const char* aKey)
        {
            auto it = aObj.find(aKey);
            if (it == aObj.end() || !it->is_string()) { return std::string(); }
            return std::move(it->get_ref<std::string&>());
        }

        bool BoolOf(const Json& aObj, const char* aKey, bool aDefault = false)
        {
            auto it = aObj.find(aKey);
            if (it == aObj.end() || !it->is_boolean()) { return aDefault; }
            return it->get<bool>();
        }

        int IntOf(const Json& aObj, const char* aKey, int aDefault)
        {
            auto it = aObj.find(aKey);
            if (it == aObj.end()) { return aDefault; }

            /* Clamp before narrowing: casting an out-of-range double or a NaN to int is undefined
               behaviour, and the wire can carry `1e30`. */
            constexpr double lo = (double)std::numeric_limits<int>::min();
            constexpr double hi = (double)std::numeric_limits<int>::max();

            if (it->is_number_integer())
            {
                long long v = it->get<long long>();
                if (v < (long long)std::numeric_limits<int>::min()) { return std::numeric_limits<int>::min(); }
                if (v > (long long)std::numeric_limits<int>::max()) { return std::numeric_limits<int>::max(); }
                return (int)v;
            }

            if (it->is_number_float())
            {
                double d = it->get<double>();
                if (!(d >= lo)) { return std::numeric_limits<int>::min(); }   /* also catches NaN */
                if (d > hi)     { return std::numeric_limits<int>::max(); }
                return (int)d;
            }

            return aDefault;
        }

        ///--------------------------------------------------------------------------------------------
        /// Inbound sub-parsers
        ///--------------------------------------------------------------------------------------------

        /* The list `settings` blob is opaque and the website is free to add to it, so read the
           four keys this module uses and ignore everything else. A key that is absent, null, or
           the wrong type falls back to its default. */
        void ApplySettings(const Json& aSettings, List& aOutList)
        {
            if (!aSettings.is_object()) { return; }

            aOutList.AccountName = StrOf(aSettings, "gw2AccountName");
            aOutList.Color       = StrOf(aSettings, "color");
            aOutList.Reset       = StrOf(aSettings, "reset");

            /* On the wire this is a string enum, "STATIC" or "GROUP_COMPLETED". All we need is
               whether grouping is on, so it collapses to a bool and anything else means false.
               A plain bool is accepted too, in case the website ever simplifies the field. */
            auto it = aSettings.find("sortEntries");
            if (it != aSettings.end())
            {
                if      (it->is_string())  { aOutList.SortEntries = (it->get_ref<const std::string&>() == "GROUP_COMPLETED"); }
                else if (it->is_boolean()) { aOutList.SortEntries = it->get<bool>(); }
            }
        }

        void ParseEntries(const Json& aEntries, List& aOutList)
        {
            if (!aEntries.is_array()) { return; }

            aOutList.Entries.reserve(aEntries.size());
            for (const Json& e : aEntries)
            {
                if (!e.is_object()) { continue; }

                Entry entry;
                entry.Name          = StrOf(e, "name");
                entry.EntryType     = StrOf(e, "entry_type");
                entry.Completed     = BoolOf(e, "completed");
                entry.AutoCompleted = BoolOf(e, "autoCompleted");
                entry.HasHoverCard  = BoolOf(e, "has_hover_card");

                /* Links arrive on `entry` messages today, not in `state`. Read them here anyway
                   so a website that starts sending them just works. */
                entry.ChatLink = StrOf(e, "chat_link");
                entry.Link     = StrOf(e, "link");

                aOutList.Entries.push_back(std::move(entry));
            }
        }

        void ParseState(const Json& aRoot, StateMsg& aOut)
        {
            /* A `state` with no `protocol` means version 1. Deciding whether that version is
               acceptable belongs to the transport, which owns the close codes. */
            aOut.ProtocolVersion = IntOf(aRoot, "protocol", 1);

            auto lists = aRoot.find("lists");
            if (lists == aRoot.end() || !lists->is_array()) { return; }

            aOut.Lists.reserve(lists->size());
            for (const Json& l : *lists)
            {
                if (!l.is_object()) { continue; }

                List list;
                list.Id        = StrOf(l, "id");
                list.Name      = StrOf(l, "name");
                list.IsLootBag = BoolOf(l, "is_loot_bag");

                auto settings = l.find("settings");
                if (settings != l.end()) { ApplySettings(*settings, list); }

                auto entries = l.find("entries");
                if (entries != l.end()) { ParseEntries(*entries, list); }

                aOut.Lists.push_back(std::move(list));
            }
        }

        bool ParseEntryMsg(Json& aRoot, EntryMsg& aOut)
        {
            aOut.ListId = StrOf(aRoot, "listId");
            /* Every other field falls back to a default, but a message we cannot route is dropped. */
            if (aOut.ListId.empty()) { return false; }

            aOut.Index         = IntOf(aRoot, "index", 0);
            aOut.Name          = StrOf(aRoot, "name");
            aOut.ChatLink      = StrOf(aRoot, "chat_link");
            aOut.Link          = StrOf(aRoot, "link");
            aOut.Mime          = StrOf(aRoot, "mime");
            aOut.Completed     = BoolOf(aRoot, "completed");
            aOut.AutoCompleted = BoolOf(aRoot, "autoCompleted");
            aOut.HasHoverCard  = BoolOf(aRoot, "has_hover_card");

            /* No value at all means "flags only, leave the image alone". An empty string means
               something different: the website is telling us this row has no image. */
            auto img = aRoot.find("image_b64");
            if (img != aRoot.end() && img->is_string())
            {
                aOut.ImageB64 = std::move(img->get_ref<std::string&>());
            }

            return true;
        }

        void ParseSynced(const Json& aRoot, SyncedMsg& aOut)
        {
            /* Missing or null means an empty list, not an error. */
            auto ids = aRoot.find("listIds");
            if (ids == aRoot.end() || !ids->is_array()) { return; }

            aOut.ListIds.reserve(ids->size());
            for (const Json& id : *ids)
            {
                if (id.is_string()) { aOut.ListIds.push_back(id.get<std::string>()); }
            }
        }

        bool ParseHoverImage(Json& aRoot, HoverImgMsg& aOut)
        {
            aOut.ListId = StrOf(aRoot, "listId");
            /* Every other field falls back to a default, but a message we cannot route is dropped. */
            if (aOut.ListId.empty()) { return false; }

            aOut.Index    = IntOf(aRoot, "index", 0);
            aOut.Mime     = StrOf(aRoot, "mime");
            aOut.ImageB64 = TakeStrOf(aRoot, "image_b64");
            return true;
        }

        ///--------------------------------------------------------------------------------------------
        /// Outbound helper
        ///--------------------------------------------------------------------------------------------

        /* `replace` keeps dump() from throwing on a string the website sent us that is not valid
           UTF-8. List ids round-trip through here. */
        std::string Dump(const OutJson& aMsg)
        {
            return aMsg.dump(-1, ' ', false, OutJson::error_handler_t::replace);
        }
    }

    ///------------------------------------------------------------------------------------------------
    /// Inbound: website -> module
    ///
    /// A crash here takes Guild Wars 2 down with it, so no exception may leave Parse(). Anything
    /// malformed comes back as Kind::Unknown and the transport decides what to do about it.
    ///------------------------------------------------------------------------------------------------
    Inbound Parse(const std::string& aJson)
    {
        Inbound msg;

        try
        {
            Json root = Json::parse(aJson, nullptr, /* allow_exceptions */ false);
            if (root.is_discarded() || !root.is_object()) { return msg; }

            const std::string type = StrOf(root, "type");
            if (type.empty()) { return msg; }

            if (type == "state")
            {
                ParseState(root, msg.State);
                msg.Type = Kind::State;
            }
            else if (type == "entry")
            {
                if (ParseEntryMsg(root, msg.EntryData)) { msg.Type = Kind::Entry; }
            }
            else if (type == "synced")
            {
                ParseSynced(root, msg.Synced);
                msg.Type = Kind::Synced;
            }
            else if (type == "hover_image")
            {
                if (ParseHoverImage(root, msg.HoverImage)) { msg.Type = Kind::HoverImage; }
            }
        }
        catch (const std::exception&)
        {
            /* Backstop: the accessors above do not throw, but a huge payload can still bad_alloc. */
            return Inbound();
        }
        catch (...)
        {
            return Inbound();
        }

        return msg;
    }

    ///------------------------------------------------------------------------------------------------
    /// Outbound: module -> website
    ///------------------------------------------------------------------------------------------------

    std::string BuildSubscribe(const std::vector<std::string>& aListIds, int aRenderWidth)
    {
        try
        {
            OutJson msg;
            msg["type"]    = "subscribe";
            msg["listIds"] = aListIds;   /* always present, never null; empty means "unsubscribe all" */

            /* Omitted when we have no width to ask for, so an older website sees byte-identical
               traffic. */
            if (aRenderWidth > 0) { msg["render_width"] = aRenderWidth; }

            /* Tells the website which module is answering. `subscribe` is the first thing we send
               after the handshake, so this is the earliest it can learn that. Addon::MODULE_ID
               explains why the website cannot work it out on its own. */
            msg["module"] = Addon::MODULE_ID;

            return Dump(msg);
        }
        catch (...)
        {
            return "{\"type\":\"subscribe\",\"listIds\":[]}";
        }
    }

    std::string BuildSetEntryCompleted(const std::string& aListId, int aIndex, bool aCompleted)
    {
        try
        {
            OutJson msg;
            msg["type"]      = "set_entry_completed";
            msg["listId"]    = aListId;
            msg["index"]     = aIndex;
            msg["completed"] = aCompleted;
            return Dump(msg);
        }
        catch (...)
        {
            return std::string();
        }
    }

    std::string BuildOpenHover(const std::string& aListId, int aIndex)
    {
        try
        {
            OutJson msg;
            msg["type"]   = "open_hover";
            msg["listId"] = aListId;
            msg["index"]  = aIndex;
            return Dump(msg);
        }
        catch (...)
        {
            return std::string();
        }
    }

    std::string BuildCloseHover(const std::string& aListId, int aIndex)
    {
        /* At most one hover subscription is open at a time, so `close_hover` needs no addressee
           and carries only its type. The arguments are here for the caller's own bookkeeping. */
        (void)aListId;
        (void)aIndex;
        return "{\"type\":\"close_hover\"}";
    }
}
