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
    },
    "/auth/status": {
      "get": {
        "summary": "Check authentication status",
        "description": "Verify if the current JWT token is valid",
        "tags": ["Authentication"],
        "security": [{"bearerAuth": []}],
        "responses": {
          "200": {
            "description": "Token is valid",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "boolean"},
                    "message": {"type": "string"}
                  }
                }
              }
            }
          },
          "401": {"description": "Invalid or expired token"}
        }
      }
    },
    "/api/status": {
      "get": {
        "summary": "Get server status metrics",
        "description": "Retrieve real-time server status including file counts and processing statistics",
        "tags": ["Server Status"],
        "security": [{"bearerAuth": []}],
        "responses": {
          "200": {
            "description": "Server status retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "string", "example": "success"},
                    "data": {
                      "type": "object",
                      "properties": {
                        "files_scanned": {"type": "integer", "description": "Total files discovered and stored"},
                        "files_queued": {"type": "integer", "description": "Files waiting to be processed"},
                        "files_processed": {"type": "integer", "description": "Files successfully processed in any mode"},
                        "duplicates_found": {"type": "integer", "description": "Files identified as duplicates"}
                      }
                    }
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"},
          "500": {"description": "Internal server error"}
        }
      }
    },
    "/duplicates/find": {
      "post": {
        "summary": "Find duplicate files",
        "description": "Search for duplicate files in the scanned files database",
        "tags": ["Duplicates"],
        "security": [{"bearerAuth": []}],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "query": {"type": "string", "description": "Search query for file names"},
                  "mode": {"type": "string", "enum": ["FAST", "BALANCED", "QUALITY"], "description": "Processing mode to search in"}
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Duplicate search results",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "string"},
                    "results": {"type": "array", "items": {"type": "object"}}
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"},
          "500": {"description": "Search failed"}
        }
      }
    },
    "/config": {
      "get": {
        "summary": "Get server configuration",
        "description": "Retrieve current server configuration settings",
        "tags": ["Configuration"],
        "security": [{"bearerAuth": []}],
        "responses": {
          "200": {
            "description": "Configuration retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "string"},
                    "config": {"type": "object"}
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"}
        }
      },
      "put": {
        "summary": "Update server configuration",
        "description": "Update server configuration settings",
        "tags": ["Configuration"],
        "security": [{"bearerAuth": []}],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "dedup_mode": {"type": "string", "enum": ["FAST", "BALANCED", "QUALITY"]},
                  "log_level": {"type": "string", "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"]},
                  "server_port": {"type": "integer"},
                  "max_processing_threads": {"type": "integer"}
                }
              }
            }
          }
        },
        "responses": {
          "200": {"description": "Configuration updated successfully"},
          "401": {"description": "Authentication required"},
          "400": {"description": "Invalid configuration"},
          "500": {"description": "Update failed"}
        }
      }
    },
    "/scan": {
      "post": {
        "summary": "Scan directory",
        "description": "Scan a directory and add files to the database",
        "tags": ["Scanning"],
        "security": [{"bearerAuth": []}],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "directory_path": {"type": "string", "description": "Path to directory to scan"},
                  "recursive": {"type": "boolean", "description": "Scan subdirectories recursively"}
                },
                "required": ["directory_path"]
              }
            }
          }
        },
        "responses": {
          "200": {"description": "Scan completed successfully"},
          "401": {"description": "Authentication required"},
          "400": {"description": "Invalid directory path"},
          "500": {"description": "Scan failed"}
        }
      }
    },
    "/api/scan/targets": {
      "get": {
        "summary": "Get scan targets",
        "description": "Retrieve a list of directories configured for scanning",
        "tags": ["Scanning"],
        "security": [{"bearerAuth": []}],
        "responses": {
          "200": {
            "description": "Scan targets retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "total_targets": {"type": "integer"},
                    "database_path": {"type": "string"},
                    "scan_targets": {
                      "type": "array",
                      "items": {
                        "type": "object",
                        "properties": {
                          "path": {"type": "string"},
                          "type": {"type": "string"},
                          "status": {"type": "string"}
                        }
                      }
                    }
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"},
          "500": {"description": "Failed to retrieve scan targets"}
        }
      }
    },
    "/api/database/hash": {
      "get": {
        "summary": "Get database hash",
        "description": "Retrieve a SHA256 hash of all database table contents combined",
        "tags": ["Database"],
        "security": [{"bearerAuth": []}],
        "responses": {
          "200": {
            "description": "Database hash retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "string", "example": "success"},
                    "database_hash": {"type": "string", "description": "SHA256 hash of all database contents"}
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"},
          "500": {"description": "Failed to get database hash"}
        }
      }
    },
    "/api/database/table/{table_name}/hash": {
      "get": {
        "summary": "Get table hash",
        "description": "Retrieve a SHA256 hash of a specific database table's contents",
        "tags": ["Database"],
        "security": [{"bearerAuth": []}],
        "parameters": [
          {
            "name": "table_name",
            "in": "path",
            "required": true,
            "schema": {
              "type": "string"
            },
            "description": "Name of the database table to hash"
          }
        ],
        "responses": {
          "200": {
            "description": "Table hash retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {"type": "string", "example": "success"},
                    "table_name": {"type": "string", "description": "Name of the table that was hashed"},
                    "table_hash": {"type": "string", "description": "SHA256 hash of the table contents"}
                  }
                }
              }
            }
          },
          "401": {"description": "Authentication required"},
          "500": {"description": "Failed to get table hash"}
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
                ]
            });
        };
    </script>
</body>
</html>
    )";
  }
};