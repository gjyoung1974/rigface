// Generated from style.css by CMake; edit that file instead.
#pragma once

namespace rigface {
inline constexpr char STYLE_CSS[] = R"rigfacecss(window.rb {
  background: #1b1d21;
  color: #dfe6ee;
}

.bezel { padding: 2px 4px 6px 4px; }
.brand { font-weight: 800; letter-spacing: 4px; color: #8f99a6; font-size: 12px; }
.status { color: #8f99a6; font-size: 12px; }

.screen {
  background: #04060a;
  border: 2px solid #000000;
  border-radius: 8px;
  box-shadow: inset 0 0 0 1px #2b3039;
  padding: 8px 10px;
}

.badge {
  font-weight: 700;
  font-size: 12px;
  padding: 2px 9px;
  border-radius: 3px;
  background: #18202b;
  color: #9fb6d4;
}
.badge.rx   { background: #0e7a3a; color: #ffffff; }
.badge.tx   { background: #d21f1f; color: #ffffff; }
.badge.mode { background: #1f5fbf; color: #ffffff; }
.badge.warn { background: #c27500; color: #ffffff; }
.badge.off  { background: #10141b; color: #3d4756; }
.clock { color: #c9d3df; font-size: 13px; font-weight: 600; }
.vfo  { color: #6fb2ff; font-weight: 800; font-size: 13px; }
.sub  { color: #7d8896; font-size: 12px; font-weight: 600; }

button.key {
  background-image: linear-gradient(#3b4049, #2a2e35);
  color: #dfe6ee;
  border: 1px solid #0b0c0f;
  border-radius: 5px;
  padding: 3px 6px;
  min-height: 34px;
  min-width: 30px;
  font-weight: 700;
  font-size: 13px;
  box-shadow: inset 0 1px rgba(255, 255, 255, 0.08);
  text-shadow: none;
  outline: none;
}
button.key:hover  { background-image: linear-gradient(#474d58, #31363e); }
button.key:active { background-image: linear-gradient(#24282e, #24282e); }
button.key.on {
  background-image: linear-gradient(#3b8be6, #1d5fb0);
  border-color: #0c2c57;
  color: #ffffff;
}
button.key.tx { min-height: 44px; font-size: 15px; letter-spacing: 1px; }
button.key.tx.on {
  background-image: linear-gradient(#f04040, #b01818);
  border-color: #500808;
}
button.key.tune.on {
  background-image: linear-gradient(#f0a020, #b87008);
  border-color: #503000;
}
button.key .cap { font-size: 9px; font-weight: 700; letter-spacing: 1px; color: #8d98a6; }
button.key.on .cap { color: #cfe2fa; }
button.key .val { font-size: 12px; font-weight: 700; }

.rowcap { font-size: 10px; font-weight: 800; letter-spacing: 1px; color: #77818e; min-width: 40px; }
.knobval { font-size: 12px; font-weight: 700; color: #c9d3df; min-width: 52px; }

scale { padding: 4px 0; min-width: 140px; }
scale trough {
  background: #0b0d11;
  border: 1px solid #000000;
  border-radius: 4px;
  min-height: 6px;
}
scale highlight { background: #2f7fd8; border-radius: 4px; }
scale slider {
  background: linear-gradient(#e6ebf1, #aab3bf);
  border: 1px solid #0b0c0f;
  border-radius: 9px;
  min-width: 18px;
  min-height: 18px;
  margin: -7px;
  box-shadow: none;
}

menubutton.conn > button {
  background-image: linear-gradient(#3b4049, #2a2e35);
  color: #dfe6ee;
  border: 1px solid #0b0c0f;
  border-radius: 5px;
  padding: 2px 10px;
  font-size: 12px;
  font-weight: 700;
}
popover.connpop contents { background: #23262b; color: #dfe6ee; padding: 10px; }
popover.connpop entry { background: #0b0d11; color: #ffffff; min-height: 30px; }

.setcap { font-size: 13px; font-weight: 700; color: #c9d3df; }
.hint { font-size: 11px; color: #7d8896; }
window.settings button.key { padding: 3px 16px; }
)rigfacecss";
}
