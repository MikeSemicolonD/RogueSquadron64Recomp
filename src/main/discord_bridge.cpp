// libdiscord_bridge.so: Android only, built only when the closed Discord Social SDK is found. The game loads it by name (discord_presence.cpp), so the SDK never links into the game.
#include <cstdio>
#include <memory>
#include "main/discord_presence.h"

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

namespace {

std::shared_ptr<discordpp::Client> g_client;

}

// No account linking: with an application id and no Connect, the SDK hands the activity to the Discord app on the phone.
extern "C" __attribute__((visibility("default"))) void rs64_discord_set(const Rs64DiscordStatus* s)
{

    if (!s && !g_client)
    {
        return;
    }

    if (!g_client) {
        g_client = std::make_shared<discordpp::Client>();
        g_client->SetApplicationId(s->app_id);

        fprintf(stderr, "[discord] SDK %d.%d.%d app=%llu\n", discordpp::Client::GetVersionMajor(), discordpp::Client::GetVersionMinor(), discordpp::Client::GetVersionPatch(), (unsigned long long)g_client->GetApplicationId());
    }

    if (!s) {
        g_client->ClearRichPresence();

        return;
    }

    discordpp::Activity act;
    discordpp::ActivityTimestamps ts;
    discordpp::ActivityAssets assets;

    // Without account linking Discord does not look the app up, so the name is sent too.
    act.SetName(s->name);
    act.SetType(discordpp::ActivityTypes::Playing);
    act.SetDetails(s->details);

    if (*s->state) {
        act.SetState(s->state);
    }

    ts.SetStart((uint64_t)s->start * 1000);
    act.SetTimestamps(ts);
    assets.SetLargeImage(s->large_image);
    assets.SetLargeText(s->large_text);
    act.SetAssets(assets);

    if (s->players > 0) {
        discordpp::ActivityParty party;

        party.SetId("rs64-coop");
        party.SetCurrentSize(s->players);
        party.SetMaxSize(s->max_players);
        act.SetParty(party);
    }

    g_client->UpdateRichPresence(act, [](discordpp::ClientResult r) {
        if (!r.Successful()) {
            fprintf(stderr, "[discord] UpdateRichPresence failed: %s\n", r.ToString().c_str());
        }
    });
}

// The SDK delivers its replies only from RunCallbacks.
extern "C" __attribute__((visibility("default"))) void rs64_discord_pump()
{

    if (g_client) {
        discordpp::RunCallbacks();
    }
}
