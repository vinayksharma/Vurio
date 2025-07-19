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
    "description": "A secure dedup service with JWT authentication"
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
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "username": {
                    "type": "string"
                  },
                  "password": {
                    "type": "string"
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
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "401": {
            "description": "Invalid credentials"
          }
        }
      }
    },
    "/auth/status": {
      "get": {
        "summary": "Check service status",
        "responses": {
          "200": {
            "description": "Service status",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "status": {
                      "type": "boolean"
                    }
                  }
                }
              }
            }
          },
          "401": {
            "description": "Unauthorized"
          },
          "500": {
            "description": "Internal server error"
          }
        }
      }
    },
    "/duplicates/find": {
      "post": {
        "summary": "Find duplicates in directory",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "directory": {
                    "type": "string",
                    "description": "Directory path to scan for duplicates"
                  }
                },
                "required": ["directory"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Directory scan completed",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "message": {
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Invalid request"
          },
          "401": {
            "description": "Unauthorized"
          },
          "500": {
            "description": "Directory scan failed"
          }
        }
      }
    },
    "/config": {
      "get": {
        "summary": "Get server configuration",
        "responses": {
          "200": {
            "description": "Server configuration",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "dedup_mode": {
                      "type": "string",
                      "enum": ["FAST", "BALANCED", "QUALITY"]
                    },
                    "log_level": {
                      "type": "string",
                      "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"]
                    },
                    "server_port": {
                      "type": "integer"
                    },
                    "auth_secret": {
                      "type": "string"
                    },
                    "server_host": {
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "401": {
            "description": "Unauthorized"
          }
        }
      },
      "put": {
        "summary": "Update server configuration",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "dedup_mode": {
                    "type": "string",
                    "enum": ["FAST", "BALANCED", "QUALITY"]
                  },
                  "log_level": {
                    "type": "string",
                    "enum": ["TRACE", "DEBUG", "INFO", "WARN", "ERROR"]
                  },
                  "server_port": {
                    "type": "integer"
                  },
                  "server_host": {
                    "type": "string"
                  },
                  "auth_secret": {
                    "type": "string"
                  }
                }
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Configuration updated successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "message": {
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Invalid configuration"
          },
          "401": {
            "description": "Unauthorized"
          }
        }
      }
    },
    "/config/reload": {
      "post": {
        "summary": "Reload configuration from file",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "file_path": {
                    "type": "string",
                    "description": "Path to configuration file"
                  }
                },
                "required": ["file_path"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Configuration reloaded successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "message": {
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "400": {
            "description": "Failed to reload configuration"
          },
          "401": {
            "description": "Unauthorized"
          }
        }
      }
    },
    "/config/save": {
      "post": {
        "summary": "Save configuration to file",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "file_path": {
                    "type": "string",
                    "description": "Path to save configuration file"
                  }
                },
                "required": ["file_path"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Configuration saved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "message": {
                      "type": "string"
                    }
                  }
                }
              }
            }
          },
          "500": {
            "description": "Failed to save configuration"
          },
          "401": {
            "description": "Unauthorized"
          }
        }
      }
    },
    "/scan": {
      "post": {
        "summary": "Scan a directory and store file metadata",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "directory": {
                    "type": "string",
                    "description": "Directory path to scan"
                  },
                  "recursive": {
                    "type": "boolean",
                    "description": "Whether to scan recursively",
                    "default": true
                  },
                  "database_path": {
                    "type": "string",
                    "description": "Path to the scan results database file",
                    "default": "scan_results.db"
                  }
                },
                "required": ["directory"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Directory scan completed",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "message": { "type": "string" },
                    "files_scanned": { "type": "integer" },
                    "database_path": { "type": "string" },
                    "warning": { "type": "string" }
                  }
                }
              }
            }
          },
          "400": { "description": "Invalid request" },
          "401": { "description": "Unauthorized" },
          "500": { "description": "Directory scan failed" }
        }
      }
    },
    "/scan/results": {
      "get": {
        "summary": "Get scanned file metadata from the database",
        "parameters": [
          {
            "name": "database_path",
            "in": "query",
            "required": false,
            "schema": { "type": "string", "default": "scan_results.db" },
            "description": "Path to the scan results database file"
          }
        ],
        "responses": {
          "200": {
            "description": "Scan results retrieved successfully",
            "content": {
              "application/json": {
                "schema": {
                  "type": "object",
                  "properties": {
                    "total_files": { "type": "integer" },
                    "database_path": { "type": "string" },
                    "files": {
                      "type": "array",
                      "items": {
                        "type": "object",
                        "properties": {
                          "file_path": { "type": "string" },
                          "file_name": { "type": "string" }
                        }
                      }
                    }
                  }
                }
              }
            }
          },
          "401": { "description": "Unauthorized" },
          "500": { "description": "Failed to retrieve scan results" }
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
                <title>Dedup API Documentation</title>
                <link rel="stylesheet" type="text/css" href="https://unpkg.com/swagger-ui-dist@4/swagger-ui.css">
                <script src="https://unpkg.com/swagger-ui-dist@4/swagger-ui-bundle.js"></script>
            </head>
            <body>
                <div id="swagger-ui"></div>
                <script>
                    window.onload = function() {
                        SwaggerUIBundle({
                            url: ")" +
           std::string(ServerConfig::SWAGGER_JSON_PATH) + R"(",
                            dom_id: '#swagger-ui',
                            deepLinking: true,
                            presets: [
                                SwaggerUIBundle.presets.apis,
                                SwaggerUIBundle.SwaggerUIStandalonePreset
                            ],
                        });
                    };
                </script>
            </body>
            </html>
        )";
  }
};