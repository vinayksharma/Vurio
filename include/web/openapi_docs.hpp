#pragma once

#include <string>
#include "core/server_config.hpp"

class OpenApiDocs
{
public:
  static const std::string &getSpec()
  {
    static const std::string spec = R"({
  "openapi": "3.0.0",
  "info": {
    "title": "Dedup API",
    "version": "1.0.0",
    "description": "A secure dedup service with JWT authentication for media file deduplication and processing"
  },
  "servers": [
    {
      "url": ")" + ServerConfig::getServerUrl() +
                                    R"(",
      "description": "Local development server"
    }
  ],
  "components": {
    "securitySchemes": {
      "bearerAuth": {
        "type": "http",
        "scheme": "bearer",
        "bearerFormat": "JWT"
      }
    }
  },
  "security": [
    {
      "bearerAuth": []
    }
  ],
  "paths": {
    "/auth/login": {
      "post": {
        "summary": "Login to get JWT token",
        "description": "Authenticate user and receive JWT token for API access",
        "tags": ["Authentication"],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "username": {
                    "type": "string",
                    "description": "Username for authentication"
                  },
                  "password": {
                    "type": "string",
                    "description": "Password for authentication"
                  }
                },
                "required": ["username", "password"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Successful login",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "token": {
                      "type": "string",
                      "description": "JWT token for API access"
                    },
                    "message": {
                      "type": "string",
                      "description": "Success message"
                    }
                  }
                }
              }
            }
          },
          "401": { "description": "Invalid credentials" },
          "500": { "description": "Authentication failed" }
        }
      }
    }
  }
})";
    return spec;
  }

  static std::string getSwaggerUI()
  {
    return R"(
<!DOCTYPE html>
<html>
<head>
    <title>Dedup API - Swagger UI</title>
    <link rel="stylesheet" type="text/css" href="https://unpkg.com/swagger-ui-dist@5.9.0/swagger-ui.css" />
    <style>
        html { box-sizing: border-box; overflow: -moz-scrollbars-vertical; overflow-y: scroll; }
        *, *:before, *:after { box-sizing: inherit; }
        body { margin:0; background: #fafafa; }
    </style>
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://unpkg.com/swagger-ui-dist@5.9.0/swagger-ui-bundle.js"></script>
    <script src="https://unpkg.com/swagger-ui-dist@5.9.0/swagger-ui-standalone-preset.js"></script>
    <script>
        window.onload = function() {
            const ui = SwaggerUIBundle({
                url: ")" +
           std::string(ServerConfig::SWAGGER_JSON_PATH) + R"(",
                dom_id: '#swagger-ui',
                deepLinking: true,
                presets: [
                    SwaggerUIBundle.presets.apis,
                    SwaggerUIStandalonePreset
                ],
                plugins: [
                    SwaggerUIBundle.plugins.DownloadUrl
                ],
                layout: "Standalone"
            });
        };
    </script>
</body>
</html>
    )";
  }
};