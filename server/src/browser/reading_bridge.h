#pragma once
#include "ai/ai_translation.h"
namespace BrowserReading
{
void Start(const std::string &database, AiTranslation::Fetcher fetcher);
void Cancel();
void Stop();
} // namespace BrowserReading
