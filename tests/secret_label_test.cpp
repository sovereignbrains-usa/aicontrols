// How a secret is labelled in the panel. Exit code 0: all passed.
#include <iostream>
#include <string>

#include "core/secret_label.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

}  // namespace

int main() {
    const aic::SecretFace packetlab = aic::DescribeSecret("ssh root@5.83.147.210");
    const aic::SecretFace nl = aic::DescribeSecret("ssh root@94.249.230.169");
    const aic::SecretFace otherServer = aic::DescribeSecret("ssh root@10.0.0.8");
    const aic::SecretFace key = aic::DescribeSecret("id_ed25519");
    const aic::SecretFace warp = aic::DescribeSecret("WARP");
    const aic::SecretFace sub = aic::DescribeSecret("PACKETLAB_SUB");
    const aic::SecretFace github = aic::DescribeSecret("GITHUB");
    const aic::SecretFace drive = aic::DescribeSecret("GDRIVE_REFRESH_TOKEN");
    const aic::SecretFace cloud = aic::DescribeSecret("CLAUDEFLARE");
    const aic::SecretFace aeza = aic::DescribeSecret("CFTOKENAEZA");
    const aic::SecretFace groq = aic::DescribeSecret("Groq");
    const aic::SecretFace groc = aic::DescribeSecret("GROC");
    const aic::SecretFace router = aic::DescribeSecret("192.168.31.1 router");
    const aic::SecretFace other = aic::DescribeSecret("mystery");

    Expect(packetlab.group == 0 && packetlab.title == "packetlab" && packetlab.hint == "5.83.147.210" && !packetlab.leaked,
           "packetlab server");
    Expect(nl.group == 0 && nl.rank > packetlab.rank && nl.title == "Нидерланды" && nl.hint == "94.249.230.169",
           "netherlands server");
    Expect(otherServer.group == 0 && otherServer.title == "root@10.0.0.8", "an unknown server keeps user@host");
    Expect(key.group == 1 && key.heading == "Ключи SSH" && key.hint == "id_ed25519", "the ssh private key");
    Expect(warp.group == 2 && warp.leaked && sub.group == 2 && sub.leaked && warp.rank < sub.rank, "leaked subscriptions");
    Expect(aic::DescribeSecret("warp").leaked && aic::DescribeSecret("packetlab_sub").leaked, "leak mark ignores case");
    Expect(!github.leaked && !cloud.leaked && !router.leaked && !groq.leaked, "the rest are not marked leaked");
    Expect(github.group == 3 && aic::DescribeSecret("GITHUB_OAUTH_ID").group == 3 &&
               aic::DescribeSecret("GITHUB_OAUTH_SECRET").rank > aic::DescribeSecret("GITHUB_OAUTH_ID").rank,
           "github stays together");
    Expect(drive.group == 4 && drive.title == "Доступ", "drive refresh token");
    Expect(cloud.group == 5 && cloud.hint == "SovereignBrains" && aeza.group == 5 && aeza.hint == "невалиден с 29.10",
           "cloudflare tokens");
    Expect(groq.group == 6 && groc.group == 6 && groq.hint == "голос Whisper", "groq and GROC are one group");
    Expect(router.group == 7 && router.title == "Роутер" && router.hint == "192.168.31.1", "home router");
    Expect(other.group == 8 && other.title == "mystery" && other.hint.empty(), "anything else");
    Expect(packetlab.group < key.group && key.group < warp.group && warp.group < github.group &&
               github.group < drive.group && drive.group < cloud.group && cloud.group < groq.group &&
               groq.group < router.group && router.group < other.group,
           "groups stay in the panel order");
    Expect(aic::SecretHost("ssh root@10.0.0.8:2222") == "10.0.0.8", "a port is not part of the host");
    Expect(aic::DescribeSecret("ssh root@10.0.0.8:2222").title == "root@10.0.0.8:2222" &&
               aic::DescribeSecret("ssh root@10.0.0.8:2222").hint == "сервер",
           "an unknown server with a port");
    return failures == 0 ? 0 : 1;
}
