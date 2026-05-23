#include <iostream>
#include <Poco/JSON/Parser.h>

int main() {
    Poco::JSON::Parser parser;
    auto json_obj = parser.parse("{\"value\": 0.94}").extract<Poco::JSON::Object::Ptr>();
    std::cout << "Value: " << json_obj->get("value").toString() << std::endl;
    return 0;
}
