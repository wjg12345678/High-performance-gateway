#include <cassert>
#include <string>

#include "../http/core/http_message.h"

static void decodes_url_encoded_values()
{
    assert(http_core::url_decode("hello+atlas%21") == "hello atlas!");
    assert(http_core::url_decode("%2Fapi%2Fdrive%3Fq%3Da") == "/api/drive?q=a");
    assert(http_core::url_decode("bad%ZZvalue") == "bad%ZZvalue");
}

static void escapes_json_control_characters()
{
    assert(http_core::json_escape("a\"b\\c\n\t") == "a\\\"b\\\\c\\n\\t");
    assert(http_core::json_escape(std::string("x") + static_cast<char>(1) + "y") == "x y");
}

static void extracts_header_parameters()
{
    const std::string disposition = "form-data; name=\"file\"; filename=\"atlas.txt\"";
    assert(http_core::header_param_value(disposition, "name") == "file");
    assert(http_core::header_param_value(disposition, "filename") == "atlas.txt");
    assert(http_core::header_param_value("attachment; filename=report.pdf", "filename") == "report.pdf");
    assert(http_core::header_param_value("attachment", "filename").empty());
}

static void reads_request_values_and_auth_headers()
{
    http_core::HttpRequest request;
    request.headers["authorization"] = "Bearer   token-123  ";
    request.query["limit"] = "200";
    request.query["cursor"] = "not-a-number";
    request.query["public"] = "yes";
    request.form["filename"] = "form-name.txt";
    request.json["fallback"] = "json-value";

    assert(request.header_value("Authorization") == "Bearer   token-123  ");
    assert(request.bearer_token() == "token-123");
    assert(request.query_long_value("limit", 20, 1, 100) == 100);
    assert(request.query_long_value("cursor", 0, 0, 100) == 0);
    assert(request.query_truthy_value("public"));
    assert(request.value("filename") == "form-name.txt");
    assert(request.value("missing", "fallback") == "json-value");
}

static void builds_response_shapes()
{
    http_core::HttpResponse response;
    response.set_json_error(400, "Bad Request", "bad \"input\"\n");
    assert(response.status == 400);
    assert(response.content_type == "application/json");
    assert(response.body == "{\"code\":400,\"message\":\"bad \\\"input\\\"\\n\"}");

    response.set_options();
    assert(response.status == 204);
    assert(response.options_response);
    assert(!response.file.enabled);

    response.set_file("/tmp/atlas.txt", "", "atlas.txt");
    assert(response.status == 200);
    assert(response.content_type == "application/octet-stream");
    assert(response.file.enabled);
    assert(response.file.path == "/tmp/atlas.txt");
}

static void decodes_base64_payloads()
{
    std::string output;
    assert(http_core::decode_base64("YXRsYXM=", output));
    assert(output == "atlas");
    assert(!http_core::decode_base64("bad", output));
}

int main()
{
    decodes_url_encoded_values();
    escapes_json_control_characters();
    extracts_header_parameters();
    reads_request_values_and_auth_headers();
    builds_response_shapes();
    decodes_base64_payloads();
    return 0;
}
