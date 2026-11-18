#pragma once

class RID;

// Native host entry. Only the fixed built-in fallback backend supports an owned font cache.
bool meton_isolate_system_font_cache(const RID &p_font);
