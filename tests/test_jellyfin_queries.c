/* Focused request-construction tests — build and run with `make test`.
 *
 * Item-list queries differ by collection type because Jellyfin can spend
 * enough time computing unused count fields to exceed MiSTerFin's request
 * timeout. Compare complete paths so changes to traversal, fields, sorting,
 * paging, user data, or image selection cannot pass unnoticed. Live TV tests
 * also pin the Jellyfin Web-style PlaybackInfo body, playback session fields,
 * URL authentication, and the narrow server compatibility workaround. */

#include <stdio.h>
#include <string.h>
#include "jellyfin.h"

static int failures;
static int checks;

#define CHECK_PATH(label, got, expected) do {                         \
    checks++;                                                         \
    if (strcmp((got), (expected)) != 0) {                             \
        failures++;                                                   \
        printf("  FAIL %s\n    expected: %s\n    got:      %s\n",    \
               (label), (expected), (got));                           \
    }                                                                 \
} while (0)

#define CHECK_TRUE(label, condition) do {                             \
    checks++;                                                         \
    if (!(condition)) {                                               \
        failures++;                                                   \
        printf("  FAIL %s\n", (label));                              \
    }                                                                 \
} while (0)

static JfConfig config(void)
{
    JfConfig cfg = {0};
    strcpy(cfg.user_id, "user-id");
    return cfg;
}

static void test_movie_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "movie-view", "movies", 12, 34,
                        path, sizeof(path));

    CHECK_PATH("movie query", path,
        "/Items?userId=user-id&ParentId=movie-view"
        "&Recursive=true&IncludeItemTypes=Movie"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=12&Limit=34");
}

static void test_music_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "music-view", "music", 5, 64,
                        path, sizeof(path));

    CHECK_PATH("music query", path,
        "/Items?userId=user-id&ParentId=music-view"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=5&Limit=64");
}

static void test_music_video_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "music-video-view", "musicvideos", 7, 50,
                        path, sizeof(path));

    CHECK_PATH("music-video query", path,
        "/Items?userId=user-id&ParentId=music-video-view"
        "&Recursive=true&IncludeItemTypes=MusicVideo"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=7&Limit=50");
}

static void test_standard_query(void)
{
    JfConfig cfg = config();
    char path[512];
    static const char expected[] =
        "/Items?userId=user-id&ParentId=tv-view"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount,RecursiveItemCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=0&Limit=128";

    jf_build_items_path(&cfg, "tv-view", "tvshows", 0, 128,
                        path, sizeof(path));
    CHECK_PATH("TV uses standard query", path, expected);

    jf_build_items_path(&cfg, "tv-view", "books", 0, 128,
                        path, sizeof(path));
    CHECK_PATH("unknown type uses standard query", path, expected);

    jf_build_items_path(&cfg, "tv-view", NULL, 0, 128,
                        path, sizeof(path));
    CHECK_PATH("missing type uses standard query", path, expected);
}

static void test_live_tv_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_live_tv_channels_path(&cfg, 9, 40, path, sizeof(path));

    CHECK_PATH("Live TV channel query", path,
        "/LiveTv/Channels?userId=user-id&StartIndex=9&Limit=40"
        "&AddCurrentProgram=true"
        "&EnableImages=true&ImageTypeLimit=1&EnableImageTypes=Primary");

    jf_build_live_tv_channels_path(&cfg, -3, 1, path, sizeof(path));
    CHECK_PATH("Live TV start index is normalized", path,
        "/LiveTv/Channels?userId=user-id&StartIndex=0&Limit=1"
        "&AddCurrentProgram=true"
        "&EnableImages=true&ImageTypeLimit=1&EnableImageTypes=Primary");
}

static void test_live_tv_playback_paths(void)
{
    JfConfig cfg = config();
    strcpy(cfg.server, "http://jellyfin");
    strcpy(cfg.token, "test-token");
    strcpy(cfg.device_id, "test-device");
    strcpy(cfg.tv_mode, "NTSC");
    char path[512];

    jf_build_live_tv_playback_info_path("channel/id", path, sizeof(path));
    CHECK_PATH("Live TV playback-info query", path,
        "/Items/channel_id/PlaybackInfo");

    JfStreamProfile profile = {640, 480, 5000000};
    char body[1536];
    CHECK_TRUE("Live TV playback-info body fits",
          jf_build_live_tv_playback_info_body(&cfg, &profile, body, sizeof(body)));
    CHECK_TRUE("Live TV profile identifies the user",
          strstr(body, "\"UserId\":\"user-id\"") != NULL);
    CHECK_TRUE("Live TV profile starts at the live edge",
          strstr(body, "\"StartTimeTicks\":0") != NULL);
    CHECK_TRUE("Live TV profile opens the stream for playback",
          strstr(body, "\"IsPlayback\":true,\"AutoOpenLiveStream\":true") != NULL);
    CHECK_TRUE("Live TV profile forces server transcoding",
          strstr(body, "\"EnableDirectPlay\":false,\"EnableDirectStream\":false,"
                       "\"EnableTranscoding\":true") != NULL);
    CHECK_TRUE("Live TV profile disables stream copies",
          strstr(body, "\"AllowVideoStreamCopy\":false,"
                       "\"AllowAudioStreamCopy\":false") != NULL);
    CHECK_TRUE("Live TV profile carries the streaming bitrate",
          strstr(body, "\"MaxStreamingBitrate\":5000000") != NULL);
    CHECK_TRUE("Live TV profile requests MPEG-TS",
          strstr(body, "\"Container\":\"ts\"") != NULL);
    CHECK_TRUE("Live TV profile requests MPEG-2 video",
          strstr(body, "\"VideoCodec\":\"mpeg2video\"") != NULL);
    CHECK_TRUE("Live TV profile omits unsupported MPEG-2 profile and level",
          strstr(body, "\"Property\":\"VideoProfile\"") == NULL &&
          strstr(body, "\"Property\":\"VideoLevel\"") == NULL);
    CHECK_TRUE("Live TV profile requests MP3 audio",
          strstr(body, "\"AudioCodec\":\"mp3\"") != NULL);
    CHECK_TRUE("Live TV profile carries width limit",
          strstr(body, "\"Property\":\"Width\",\"Value\":\"640\"") != NULL);
    CHECK_TRUE("Live TV profile carries height limit",
          strstr(body, "\"Property\":\"Height\",\"Value\":\"480\"") != NULL);
    CHECK_TRUE("Live TV profile carries NTSC frame-rate limit",
          strstr(body, "\"Property\":\"VideoFramerate\",\"Value\":\"30\"") != NULL);
    char small_body[32];
    CHECK_TRUE("Live TV profile reports a truncated destination",
          !jf_build_live_tv_playback_info_body(&cfg, &profile,
                                                small_body, sizeof(small_body)));

    strcpy(cfg.tv_mode, "PAL");
    CHECK_TRUE("Live TV PAL profile fits",
          jf_build_live_tv_playback_info_body(&cfg, &profile, body, sizeof(body)));
    CHECK_TRUE("Live TV profile carries PAL frame-rate limit",
          strstr(body, "\"Property\":\"VideoFramerate\",\"Value\":\"25\"") != NULL);

    JfLivePlayback playback = {0};
    strcpy(playback.media_source_id, "media-source");
    strcpy(playback.play_session_id, "play-session");
    memset(playback.live_stream_id, 'a', 160);
    playback.live_stream_id[160] = '\0';
    CHECK_TRUE("Live TV playstate retains a long tuner composite ID",
          jf_build_live_tv_playstate_body(&playback, "channel/id", 0, 0, -1,
                                           body, sizeof(body)) &&
          strstr(body, playback.live_stream_id) != NULL);
    strcpy(playback.live_stream_id, "live-stream");
    CHECK_TRUE("Live TV playstate body fits",
          jf_build_live_tv_playstate_body(&playback, "channel/id", 123456, 0, 1,
                                           body, sizeof(body)));
    CHECK_PATH("Live TV playstate follows Jellyfin session reporting", body,
        "{\"ItemId\":\"channel_id\",\"MediaSourceId\":\"media-source\","
        "\"LiveStreamId\":\"live-stream\",\"PlaySessionId\":\"play-session\","
        "\"PositionTicks\":123456,\"IsPaused\":false,\"PlayMethod\":\"Transcode\","
        "\"CanSeek\":false,\"Failed\":true}");
    CHECK_TRUE("Live TV progress body fits",
          jf_build_live_tv_playstate_body(&playback, "channel/id", 42, 0, -1,
                                           body, sizeof(body)));
    CHECK_TRUE("Live TV progress omits stop-only Failed",
          strstr(body, "\"Failed\"") == NULL);
    JfLivePlayback incomplete = {0};
    CHECK_TRUE("Live TV playstate rejects incomplete negotiation",
          !jf_build_live_tv_playstate_body(&incomplete, "channel/id", 0, 0, -1,
                                            body, sizeof(body)) && body[0] == '\0');

    jf_live_tv_transcoding_url(&cfg,
        "/Videos/channel-id/stream.ts?MediaSourceId=source&level=4"
        "&mpeg2video-level=4&h264-level=41&LiveStreamId=live&ApiKey=test-token",
        path, sizeof(path));
    CHECK_PATH("Live TV transcode path retains server auth and omits MPEG-2 level", path,
        "http://jellyfin/Videos/channel-id/stream.ts?MediaSourceId=source&h264-level=41"
        "&LiveStreamId=live&ApiKey=test-token");

    jf_live_tv_transcoding_url(&cfg,
        "/Videos/channel-id/stream.ts?MediaSourceId=source",
        path, sizeof(path));
    CHECK_PATH("Live TV transcode adds auth when the server omits it", path,
        "http://jellyfin/Videos/channel-id/stream.ts?MediaSourceId=source&ApiKey=test-token");

    jf_live_tv_transcoding_url(&cfg, "/Videos/channel-id/stream.ts",
                               path, sizeof(path));
    CHECK_PATH("Live TV transcode adds the first query parameter", path,
        "http://jellyfin/Videos/channel-id/stream.ts?ApiKey=test-token");

    CHECK_TRUE("Live TV transcode rejects a foreign absolute URL",
          !jf_live_tv_transcoding_url(&cfg,
              "http://other-server/Videos/channel-id/stream.ts", path, sizeof(path)));
    CHECK_TRUE("Live TV transcode rejects a missing destination",
          !jf_live_tv_transcoding_url(&cfg,
              "/Videos/channel-id/stream.ts", NULL, 0));
    char short_path[16];
    CHECK_TRUE("Live TV transcode reports a truncated destination",
          !jf_live_tv_transcoding_url(&cfg,
              "/Videos/channel-id/stream.ts", short_path, sizeof(short_path)));
}

static void test_input_normalization(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "bad/id?", "music", -7, 9,
                        path, sizeof(path));

    CHECK_PATH("parent ID and start index are normalized", path,
        "/Items?userId=user-id&ParentId=bad_id_"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=0&Limit=9");
}

static void test_collection_item_types(void)
{
    CHECK_PATH("movie collection item type",
               collection_item_type("movies"), "Movie");
    CHECK_PATH("TV collection item type",
               collection_item_type("tvshows"), "Series");
    CHECK_PATH("music collection item type",
               collection_item_type("music"), "MusicAlbum");
    CHECK_PATH("music-video collection item type",
               collection_item_type("musicvideos"), "MusicVideo");
}

static void test_live_tv_view_classification(void)
{
    JfItem server_view = {0};
    strcpy(server_view.collection_type, "livetv");
    CHECK_PATH("server Live TV view is recognized",
               view_is_live_tv(&server_view) ? "yes" : "no", "yes");

    JfItem synthetic_view = {0};
    synthetic_view.synthetic = JF_SYNTH_LIVE_TV;
    CHECK_PATH("synthetic Live TV view is recognized",
               view_is_live_tv(&synthetic_view) ? "yes" : "no", "yes");

    JfItem movie_view = {0};
    strcpy(movie_view.collection_type, "movies");
    CHECK_PATH("movie view is not Live TV",
               view_is_live_tv(&movie_view) ? "yes" : "no", "no");
}

int main(void)
{
    test_movie_query();
    test_music_query();
    test_music_video_query();
    test_standard_query();
    test_live_tv_query();
    test_live_tv_playback_paths();
    test_input_normalization();
    test_collection_item_types();
    test_live_tv_view_classification();

    printf("jellyfin queries: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
