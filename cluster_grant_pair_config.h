#pragma once
// v87-beta129: PRIVATE provisioning inputs. Keep this file OUT of public repositories.
// Fill in the SAME randomly generated 64 hexadecimal digits (256 bits) on
// devfreenove1 and devxiao3 before building. Empty = fail closed (no SF-TG traffic).
// Do NOT reuse the cluster password or publish the filled-in header.
#define SF_GRANT_PAIR_KEY_HEX ""
// Pinned integration IDs for the one explicitly enrolled coordinator/broadcaster pair.
// Change only after an intentional out-of-band, authenticated re-enrollment.
#define SF_GRANT_COORDINATOR_ID "sf-0786bb388f7381755ea6eec9b12f9a45"
#define SF_GRANT_BROADCASTER_ID "sf-a2c742fbcc9b681df371850757b16ef0"
