/* Unit tests for Jellyfin item and image query builders — run with `make test`.
 *
 * Item-list queries differ by collection type because Jellyfin can spend
 * enough time computing unused count fields to exceed MiSTerFin's request
 * timeout. Compare complete paths so changes to traversal, fields, sorting,
 * paging, user data, or image selection cannot pass unnoticed. */

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

#define CHECK_NULL(label, got) do {                                   \
    const char *value = (got);                                        \
    checks++;                                                         \
    if (value != NULL) {                                              \
        failures++;                                                   \
        printf("  FAIL %s\n    expected: NULL\n    got:      %s\n",  \
               (label), value);                                      \
    }                                                                 \
} while (0)

#define CHECK_INT(label, got, expected) do {                          \
    int value = (got);                                                \
    checks++;                                                         \
    if (value != (expected)) {                                        \
        failures++;                                                   \
        printf("  FAIL %s\n    expected: %d\n    got:      %d\n",  \
               (label), (expected), value);                           \
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

static void test_home_video_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "home-video-view", "homevideos", 3, 40,
                        path, sizeof(path));

    CHECK_PATH("home-video query", path,
        "/Items?userId=user-id&ParentId=home-video-view"
        "&IncludeItemTypes=Folder,PhotoAlbum,Video,Photo"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=false"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=3&Limit=40");
}

static void test_mixed_query(void)
{
    JfConfig cfg = config();
    char path[512];

    jf_build_items_path(&cfg, "mixed-view", "mixed", 6, 48,
                        path, sizeof(path));

    CHECK_PATH("mixed query", path,
        "/Items?userId=user-id&ParentId=mixed-view"
        "&IncludeItemTypes=Folder,PhotoAlbum,Movie,Series,Season,Episode,Video,MusicVideo,"
        "Audio,MusicAlbum,MusicArtist,Photo,Book,AudioBook,BoxSet,Playlist,Trailer,Recording"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=false"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=6&Limit=48");
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

    jf_build_items_path(&cfg, "tv-view", "plugin-defined", 0, 128,
                        path, sizeof(path));
    CHECK_PATH("unknown type uses standard query", path, expected);

    jf_build_items_path(&cfg, "tv-view", NULL, 0, 128,
                        path, sizeof(path));
    CHECK_PATH("missing type uses standard query", path, expected);
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

static void test_photo_image_query(void)
{
    JfConfig cfg = config();
    JfItem item = {0};
    char url[512] = "";
    strcpy(cfg.server, "http://jellyfin.test");
    strcpy(item.id, "photo/id?");
    strcpy(item.image_tag, "tag&value");

    CHECK_INT("photo image URL is available",
              jf_photo_image_url(&cfg, &item, 640, 288, url, sizeof(url)), 1);
    CHECK_PATH("photo image query", url,
        "http://jellyfin.test/Items/photo_id_/Images/Primary"
        "?tag=tag_value&maxWidth=640&maxHeight=288&quality=90&format=Jpg");

    item.image_tag[0] = '\0';
    CHECK_INT("photo without an image tag is rejected",
              jf_photo_image_url(&cfg, &item, 640, 288, url, sizeof(url)), 0);
    strcpy(item.image_tag, "tag");
    CHECK_INT("photo without bounded dimensions is rejected",
              jf_photo_image_url(&cfg, &item, 640, 0, url, sizeof(url)), 0);
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
    CHECK_PATH("home-video collection item types",
               collection_item_type("homevideos"), "Video,Photo");
    CHECK_PATH("mixed collection item types",
               collection_item_type("mixed"),
               "Movie,Series,Video,MusicVideo,Audio,Photo");
    CHECK_NULL("unknown collection item type",
               collection_item_type("plugin-defined"));
    CHECK_NULL("missing collection item type", collection_item_type(NULL));
}

int main(void)
{
    test_movie_query();
    test_music_query();
    test_music_video_query();
    test_home_video_query();
    test_mixed_query();
    test_standard_query();
    test_input_normalization();
    test_photo_image_query();
    test_collection_item_types();

    printf("jellyfin queries: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
