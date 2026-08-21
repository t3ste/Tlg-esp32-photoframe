#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "headlines.h"
}

namespace
{

headlines_result_t extract(const char *body)
{
    headlines_result_t out;
    headlines_extract(body, strlen(body), HEADLINE_MAX_COUNT, &out);
    return out;
}

}  // namespace

TEST(HeadlinesExtract, PlainRssTitles)
{
    const char *feed =
        "<rss><channel>"
        "<item><title>First headline</title></item>"
        "<item><title>Second headline</title></item>"
        "<item><title>Third headline</title></item>"
        "</channel></rss>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 3);
    EXPECT_STREQ(out.titles[0], "First headline");
    EXPECT_STREQ(out.titles[1], "Second headline");
    EXPECT_STREQ(out.titles[2], "Third headline");
}

TEST(HeadlinesExtract, RespectsMaxCount)
{
    const char *feed =
        "<rss><channel>"
        "<item><title>One</title></item>"
        "<item><title>Two</title></item>"
        "<item><title>Three</title></item>"
        "<item><title>Four</title></item>"
        "</channel></rss>";

    headlines_result_t out;
    esp_err_t err = headlines_extract(feed, strlen(feed), 2, &out);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_EQ(out.count, 2);
    EXPECT_STREQ(out.titles[0], "One");
    EXPECT_STREQ(out.titles[1], "Two");
}

TEST(HeadlinesExtract, ClampsMaxCountAboveCap)
{
    const char *feed =
        "<item><title>A</title></item><item><title>B</title></item>"
        "<item><title>C</title></item><item><title>D</title></item>";

    headlines_result_t out;
    headlines_extract(feed, strlen(feed), 99, &out);
    EXPECT_EQ(out.count, HEADLINE_MAX_COUNT);
}

TEST(HeadlinesExtract, CdataWrappedTitle)
{
    const char *feed = "<item><title><![CDATA[Breaking: <Markets> & Politics]]></title></item>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    // '<'/'>' inside CDATA are literal text, not markup, and are not HTML
    // entities either - passed through as-is.
    EXPECT_STREQ(out.titles[0], "Breaking: <Markets> & Politics");
}

TEST(HeadlinesExtract, DecodesHtmlEntities)
{
    const char *feed =
        "<item><title>Tom &amp; Jerry: 5 &lt;things&gt; you didn&#39;t know &quot;today&quot;"
        "</title></item>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Tom & Jerry: 5 <things> you didn't know \"today\"");
}

TEST(HeadlinesExtract, AtomEntryFallback)
{
    const char *feed = "<feed><entry><title>Atom headline</title></entry></feed>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Atom headline");
}

TEST(HeadlinesExtract, MixedRssAndAtomInOneDocument)
{
    // Not realistic (a feed is one format or the other), but exercises the
    // "whichever opening tag comes first" scan logic robustly.
    const char *feed =
        "<item><title>RSS one</title></item>"
        "<entry><title>Atom one</title></entry>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 2);
    EXPECT_STREQ(out.titles[0], "RSS one");
    EXPECT_STREQ(out.titles[1], "Atom one");
}

TEST(HeadlinesExtract, GermanUmlautsTransliterated)
{
    const char *feed = "<item><title>Grosse \xc3\x9c" "berraschung in M\xc3\xbcnchen</title></item>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Grosse Ueberraschung in Muenchen");
}

TEST(HeadlinesExtract, CollapsesWhitespaceAndTrims)
{
    const char *feed = "<item><title>\n   Multi\n   line\t title   \n</title></item>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Multi line title");
}

TEST(HeadlinesExtract, SkipsItemsWithNoTitle)
{
    const char *feed =
        "<item><description>no title here</description></item>"
        "<item><title>Has a title</title></item>";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Has a title");
}

TEST(HeadlinesExtract, IncompleteTrailingBlockStopsGracefully)
{
    // Simulates a response truncated mid-item - the extractor should keep
    // whatever full items it already found instead of erroring out.
    const char *feed =
        "<item><title>Complete headline</title></item>"
        "<item><title>Cut off mid";

    headlines_result_t out = extract(feed);
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.titles[0], "Complete headline");
}

TEST(HeadlinesExtract, NoItemsReturnsFailure)
{
    const char *feed = "<rss><channel><description>Empty feed</description></channel></rss>";

    headlines_result_t out;
    esp_err_t err = headlines_extract(feed, strlen(feed), HEADLINE_MAX_COUNT, &out);
    EXPECT_NE(err, ESP_OK);
    EXPECT_EQ(out.count, 0);
}

TEST(HeadlinesExtract, TruncatesOverlyLongTitle)
{
    std::string long_title(HEADLINE_MAX_LEN + 50, 'x');
    std::string feed = "<item><title>" + long_title + "</title></item>";

    headlines_result_t out;
    headlines_extract(feed.c_str(), feed.size(), HEADLINE_MAX_COUNT, &out);
    ASSERT_EQ(out.count, 1);
    EXPECT_LT(strlen(out.titles[0]), (size_t) HEADLINE_MAX_LEN);
}
