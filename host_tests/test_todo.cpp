#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "todo.h"
}

namespace
{

todo_list_t parse(const char *body)
{
    todo_list_t out;
    todo_parse(body, strlen(body), &out);
    return out;
}

}  // namespace

TEST(TodoParse, PlainTaskNoMetadata)
{
    todo_list_t out = parse("Buy milk\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].priority, 0);
    EXPECT_STREQ(out.items[0].text, "Buy milk");
    EXPECT_STREQ(out.items[0].due_date, "");
}

TEST(TodoParse, PriorityMarker)
{
    todo_list_t out = parse("(A) Call Mom\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].priority, 'A');
    EXPECT_STREQ(out.items[0].text, "Call Mom");
}

TEST(TodoParse, CreationDateSkippedFromText)
{
    todo_list_t out = parse("(A) 2011-03-02 Call Mom\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].priority, 'A');
    EXPECT_STREQ(out.items[0].text, "Call Mom");
}

TEST(TodoParse, ProjectsAndContexts)
{
    todo_list_t out = parse("(A) 2011-03-02 Call Mom +Family @phone due:2011-03-05\n");
    ASSERT_EQ(out.count, 1);
    const todo_item_t &item = out.items[0];
    EXPECT_STREQ(item.text, "Call Mom");
    ASSERT_EQ(item.project_count, 1);
    EXPECT_STREQ(item.projects[0], "Family");
    ASSERT_EQ(item.context_count, 1);
    EXPECT_STREQ(item.contexts[0], "phone");
    EXPECT_STREQ(item.due_date, "2011-03-05");
}

TEST(TodoParse, MultipleProjectsAndContexts)
{
    todo_list_t out = parse("Plan +Trip +Vacation @home @weekend\n");
    ASSERT_EQ(out.count, 1);
    const todo_item_t &item = out.items[0];
    ASSERT_EQ(item.project_count, 2);
    EXPECT_STREQ(item.projects[0], "Trip");
    EXPECT_STREQ(item.projects[1], "Vacation");
    ASSERT_EQ(item.context_count, 2);
    EXPECT_STREQ(item.contexts[0], "home");
    EXPECT_STREQ(item.contexts[1], "weekend");
}

TEST(TodoParse, CompletedTaskExcludedByDefault)
{
    todo_list_t out = parse(
        "x 2011-03-03 2011-03-02 Review report +Project @context\n"
        "Still open task\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.items[0].text, "Still open task");
}

TEST(TodoParse, BlankAndCommentishLinesSkipped)
{
    todo_list_t out = parse("\n   \nReal task\n\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.items[0].text, "Real task");
}

TEST(TodoParse, MalformedPriorityDegradesToPlainText)
{
    // Lowercase letter inside parens isn't a valid priority marker - the
    // whole thing should just become part of the text, not crash or be
    // silently dropped.
    todo_list_t out = parse("(a) not a real priority\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].priority, 0);
    EXPECT_STREQ(out.items[0].text, "(a) not a real priority");
}

TEST(TodoParse, MalformedDateDegradesToPlainText)
{
    todo_list_t out = parse("(A) not-a-date rest of text\n");
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].priority, 'A');
    EXPECT_STREQ(out.items[0].text, "not-a-date rest of text");
}

TEST(TodoParse, EmptyFileYieldsNoItems)
{
    todo_list_t out;
    esp_err_t err = todo_parse("", 0, &out);
    EXPECT_EQ(err, ESP_ERR_INVALID_ARG);
    EXPECT_EQ(out.count, 0);
}

TEST(TodoParse, AllCompletedYieldsNoItemsButNotAnError)
{
    todo_list_t out = parse("x 2011-01-01 done one\nx 2011-01-02 done two\n");
    EXPECT_EQ(out.count, 0);
}

TEST(TodoParse, CrlfLineEndingsTolerated)
{
    todo_list_t out = parse("(A) First\r\nSecond\r\n");
    ASSERT_EQ(out.count, 2);
    EXPECT_STREQ(out.items[0].text, "First");
    EXPECT_STREQ(out.items[1].text, "Second");
}

TEST(TodoParse, NoTrailingNewlineOnLastLine)
{
    todo_list_t out = parse("Only line, no trailing newline");
    ASSERT_EQ(out.count, 1);
    EXPECT_STREQ(out.items[0].text, "Only line, no trailing newline");
}

TEST(TodoParse, CapsAtMaxItems)
{
    std::string body;
    for (int i = 0; i < TODO_MAX_ITEMS + 5; i++) {
        body += "Task\n";
    }
    todo_list_t out = parse(body.c_str());
    EXPECT_EQ(out.count, TODO_MAX_ITEMS);
}

TEST(TodoParse, TagCountCappedWithoutOverflow)
{
    std::string body = "Overloaded";
    for (int i = 0; i < TODO_TAG_MAX + 5; i++) {
        body += " +P" + std::to_string(i);
    }
    body += "\n";
    todo_list_t out = parse(body.c_str());
    ASSERT_EQ(out.count, 1);
    EXPECT_EQ(out.items[0].project_count, TODO_TAG_MAX);
}

TEST(TodoParse, NullBodyIsInvalidArg)
{
    todo_list_t out;
    esp_err_t err = todo_parse(nullptr, 0, &out);
    EXPECT_EQ(err, ESP_ERR_INVALID_ARG);
    EXPECT_EQ(out.count, 0);
}

TEST(TodoParse, NullOutIsInvalidArg)
{
    esp_err_t err = todo_parse("Task\n", 5, nullptr);
    EXPECT_EQ(err, ESP_ERR_INVALID_ARG);
}
