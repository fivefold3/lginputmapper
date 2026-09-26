// Known LG remote key codes (Linux input event codes as emitted by lginput2 /
// micomservice). Verified on a C5 with the MR25GA Magic Remote, plus codes from
// the original lginputhook "Button IDs" list and magic_mapper. Unknown codes
// are shown as "Key N" in the UI; they still work.
var KEYS = {
  2: '1', 3: '2', 4: '3', 5: '4', 6: '5', 7: '6', 8: '7', 9: '8', 10: '9', 11: '0',
  28: 'OK', 272: 'OK (wheel click)', 103: 'Up', 108: 'Down', 105: 'Left', 106: 'Right',
  412: 'Back', 174: 'Exit',
  113: 'Mute', 114: 'Volume −', 115: 'Volume +', 402: 'Channel +', 403: 'Channel −',
  398: 'Red', 399: 'Green', 400: 'Yellow', 401: 'Blue',
  773: 'Home', 1023: 'Home Hub', 1123: 'Home Hub (older remotes)', 1124: 'Accessibility',
  139: 'Settings', 241: 'Inputs', 428: 'AI / Voice', 358: 'Info', 362: 'Guide',
  994: 'Options (…)', 799: 'Options (…, IR)', 771: 'Channels', 787: 'List',
  1083: 'Search', 217: 'Search (IR)', 829: 'SAP', 1116: 'TV',
  164: 'Play/Pause', 207: 'Play', 119: 'Pause', 128: 'Stop', 167: 'Record',
  168: 'Rewind', 208: 'Fast forward', 163: 'Next track', 165: 'Previous track',
  1037: 'Netflix', 1038: 'Prime Video', 1042: 'Disney+', 1043: 'LG Channels', 1044: 'Rakuten TV',
  1111: 'Stan', 1086: 'Alexa', 1117: 'Google Assistant', 1107: 'Sling',
  // Other streaming buttons from LG's key table (they can turn the TV on too).
  1039: 'ivi', 1041: 'Hotstar', 1045: 'Globoplay', 1047: 'Okko', 1088: 'Kinopoisk', 1089: 'Watcha',
  1090: 'U-NEXT', 1091: 'FPT Play', 1092: 'Shahid', 1095: 'Hulu', 1096: 'NHK+', 1097: 'TOD',
  1099: 'Freeview Play', 1102: 'Sony LIV', 1108: 'TVer', 1109: 'Wavve', 1110: 'Coupang Play',
  1120: 'TV360', 1121: 'VTVgo', 1125: 'VK Video', 1126: 'Premier',
  1028: 'STB menu', 1021: 'STB power', 116: 'Power',
};

// Codes lginput2 emits alongside real buttons to signal pointer state; not
// useful as mappings and hidden from the key monitor by default.
var NOISE_CODES = { 1198: true, 1199: true, 1222: true };

module.exports = { KEYS: KEYS, NOISE_CODES: NOISE_CODES };
