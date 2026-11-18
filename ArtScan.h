#pragma once
// Album art survey: starts a background scan of the card and prints a report
// on Serial (lines start with "[art]"). Read-only. Serial command "artscan".
void artScanStart();

#include "Tags.h"
// Describes an image stored at [pos, pos+len) of src: returns a kind name
// ("JPEG baseline", "JPEG progressive", "PNG", ...) and its size.
const char *artDescribeImage(TagSource &src, uint32_t pos, uint32_t len, uint16_t &w, uint16_t &h);
