#pragma once


bool SendWebSocketCommand(const std::string& json, std::string& response, int timeoutMs = 2000);