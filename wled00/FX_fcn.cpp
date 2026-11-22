/*
  WS2812FX_fcn.cpp contains all utility functions
  Harm Aldick - 2016
  www.aldick.org

  Copyright (c) 2016  Harm Aldick
  Licensed under the EUPL v. 1.2 or later
  Adapted from code originally licensed under the MIT license

  Modified heavily for WLED
*/
#include "wled.h"
#include "FXparticleSystem.h"  // TODO: better define the required function (mem service) in FX.h?
#include "colors.h"

#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
static_assert(MAX_NUM_SEGMENTS <= 64, "physMap owner byte encodes at most 64 segment slots");
#endif

/*
  Custom per-LED mapping has moved!

  Create a file "ledmap.json" using the edit page.

  this is just an example (30 LEDs). It will first set all even, then all uneven LEDs.
  {"map":[
  0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28,
  1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29]}

  another example. Switches direction every 5 LEDs.
  {"map":[
  0, 1, 2, 3, 4, 9, 8, 7, 6, 5, 10, 11, 12, 13, 14,
  19, 18, 17, 16, 15, 20, 21, 22, 23, 24, 29, 28, 27, 26, 25]}
*/

static_assert(MAX_NUM_SEGMENTS >= WLED_MAX_BUSSES, "Max segments must be at least max number of busses!");


///////////////////////////////////////////////////////////////////////////////
// Segment class implementation
///////////////////////////////////////////////////////////////////////////////
unsigned      Segment::_usedSegmentData   = 0U; // amount of RAM all segments use for their data[]
uint16_t      Segment::maxWidth           = DEFAULT_LED_COUNT;
uint16_t      Segment::maxHeight          = 1;
unsigned      Segment::_vLength           = 0;
unsigned      Segment::_vWidth            = 0;
unsigned      Segment::_vHeight           = 0;
uint32_t      Segment::_currentColors[NUM_COLORS] = {0,0,0};
CRGBPalette16 Segment::_currentPalette    = CRGBPalette16();
CRGBPalette16 Segment::_randomPalette     = generateRandomPalette();  // was CRGBPalette16(DEFAULT_COLOR);
CRGBPalette16 Segment::_newRandomPalette  = generateRandomPalette();  // was CRGBPalette16(DEFAULT_COLOR);
uint16_t      Segment::_lastPaletteChange = 0; // in seconds; perhaps it should be per segment
uint16_t      Segment::_nextPaletteBlend  = 0; // in millis

bool     Segment::_modeBlend = false;
uint16_t Segment::_clipStart = 0;
uint16_t Segment::_clipStop = 0;
uint8_t  Segment::_clipStartY = 0;
uint8_t  Segment::_clipStopY = 1;

Segment::~Segment() {
  #ifdef WLED_DEBUG
  DEBUGFX_PRINTF_P(PSTR("-- Destroying segment: %p [%d,%d:%d,%d]"), this, (int)start, (int)stop, (int)startY, (int)stopY);
  if (name) DEBUGFX_PRINTF_P(PSTR(" %s (%p)"), name, name);
  if (data) DEBUGFX_PRINTF_P(PSTR(" %u->(%p)"), _dataLen, data);
  DEBUGFX_PRINTF_P(PSTR(" T[%p]"), _t);
  DEBUGFX_PRINTLN();
  #endif
  strip.invalidateStateCache();
  clearName();
  stopTransition();   // deallocate "_t" (transition) and with it "_segOld" note: _segOld has _t=null, see copy constructor
  #ifdef WLED_ENABLE_GIF
  endImagePlayback(this);
  #endif
  deallocateData();
  p_free(pixels);
}

// copy constructor
Segment::Segment(const Segment &orig) {
  //DEBUG_PRINTF_P(PSTR("-- Copy segment constructor: %p -> %p\n"), &orig, this);
  memcpy((void*)this, (void*)&orig, sizeof(Segment));
  _t   = nullptr; // copied segment cannot be in transition
  name = nullptr;
  data = nullptr;
  _dataLen = 0;
  pixels = nullptr;
  if (!stop) return;  // nothing to do if segment is inactive/invalid
  if (orig.pixels) {
    // allocate pixel buffer: prefer IRAM/PSRAM
    pixels = static_cast<uint32_t*>(allocate_buffer(orig.length() * sizeof(uint32_t), BFRALLOC_PREFER_PSRAM | BFRALLOC_NOBYTEACCESS));
    if (pixels) {
      memcpy(pixels, orig.pixels, sizeof(uint32_t) * orig.length());
      if (orig.name) { name = static_cast<char*>(allocate_buffer(strlen(orig.name)+1, BFRALLOC_PREFER_PSRAM)); if (name) strcpy(name, orig.name); }
      if (orig.data) { if (allocateData(orig._dataLen)) memcpy(data, orig.data, orig._dataLen); }
    } else {
      DEBUGFX_PRINTLN(F("!!! Not enough RAM for pixel buffer !!!"));
      errorFlag = ERR_NORAM_PX;
      stop = 0; // mark segment as inactive/invalid
    }
  } else stop = 0; // mark segment as inactive/invalid
}

// move constructor
Segment::Segment(Segment &&orig) noexcept {
  //DEBUG_PRINTF_P(PSTR("-- Move segment constructor: %p -> %p\n"), &orig, this);
  memcpy((void*)this, (void*)&orig, sizeof(Segment));
  orig._t   = nullptr; // old segment cannot be in transition any more
  orig.name = nullptr;
  orig.data = nullptr;
  orig._dataLen = 0;
  orig.pixels = nullptr;
}

// copy assignment
Segment& Segment::operator= (const Segment &orig) {
  //DEBUG_PRINTF_P(PSTR("-- Copying segment: %p -> %p\n"), &orig, this);
  if (this != &orig) {
    // clean destination
    strip.invalidateStateCache();
    if (name) { p_free(name); name = nullptr; }
    stopTransition(); // delete _t
    deallocateData();
    p_free(pixels);
    pixels = nullptr;
    // copy source
    memcpy((void*)this, (void*)&orig, sizeof(Segment));
    // erase pointers to allocated data
    data = nullptr;
    _dataLen = 0;
    if (!stop) return *this;  // nothing to do if segment is inactive/invalid
    // copy source data
    if (orig.pixels) {
      // allocate pixel buffer: prefer IRAM/PSRAM
      pixels = static_cast<uint32_t*>(allocate_buffer(orig.length() * sizeof(uint32_t), BFRALLOC_PREFER_PSRAM | BFRALLOC_NOBYTEACCESS));
      if (pixels) {
        memcpy(pixels, orig.pixels, sizeof(uint32_t) * orig.length());
        if (orig.name) { name = static_cast<char*>(allocate_buffer(strlen(orig.name)+1, BFRALLOC_PREFER_PSRAM)); if (name) strcpy(name, orig.name); }
        if (orig.data) { if (allocateData(orig._dataLen)) memcpy(data, orig.data, orig._dataLen); }
      } else {
        DEBUG_PRINTLN(F("!!! Not enough RAM for pixel buffer !!!"));
        errorFlag = ERR_NORAM_PX;
        stop = 0; // mark segment as inactive/invalid
      }
    } else stop = 0; // mark segment as inactive/invalid
  }
  return *this;
}

// move assignment
Segment& Segment::operator= (Segment &&orig) noexcept {
  //DEBUG_PRINTF_P(PSTR("-- Moving segment: %p -> %p\n"), &orig, this);
  if (this != &orig) {
    strip.invalidateStateCache();
    if (name) { p_free(name); name = nullptr; } // free old name
    stopTransition(); // delete _t
    deallocateData(); // free old runtime data
    p_free(pixels);   // free old pixel buffer
    // move source data
    memcpy((void*)this, (void*)&orig, sizeof(Segment));
    orig.name = nullptr;
    orig.data = nullptr;
    orig._dataLen = 0;
    orig.pixels = nullptr;
    orig._t = nullptr; // old segment cannot be in transition
  }
  return *this;
}

// allocates effect data buffer on heap and initialises (erases) it
bool Segment::allocateData(size_t len) {
  if (len == 0) return false;    // nothing to do
  if (data && _dataLen >= len) { // already allocated enough (reduce fragmentation)
    if (call == 0) {
        //DEBUG_PRINTF_P(PSTR("--   Clearing data (%d): %p\n"), len, this);
        memset(data, 0, len);  // erase buffer if called during effect initialisation
        return true; // no need to reallocate
    }
    else
      return true;
  }
  //DEBUG_PRINTF_P(PSTR("--   Allocating data (%d): %p\n"), len, this);
  // limit to MAX_SEGMENT_DATA if there is no PSRAM, otherwise prefer functionality over speed
  #ifndef BOARD_HAS_PSRAM
  if (Segment::getUsedSegmentData() + len - _dataLen > MAX_SEGMENT_DATA) {
    // not enough memory
    DEBUG_PRINTF_P(PSTR("SegmentData limit reached: %d/%d\n"), len, Segment::getUsedSegmentData());
    errorFlag = ERR_NORAM;
    return false;
  }
  #endif

  if (data) {
    d_free(data); // free data and try to allocate again (segment buffer may be blocking contiguous heap)
    Segment::addUsedSegmentData(-_dataLen); // subtract buffer size
  }

  data = static_cast<byte*>(allocate_buffer(len, BFRALLOC_PREFER_DRAM | BFRALLOC_CLEAR)); // prefer DRAM over PSRAM for speed

  if (data) {
    Segment::addUsedSegmentData(len);
    _dataLen = len;
    //DEBUG_PRINTF_P(PSTR("---  Allocated data (%p): %d/%d -> %p\n"), this, len, Segment::getUsedSegmentData(), data);
    return true;
  }
  // allocation failed
  DEBUG_PRINTLN(F("!!! Allocation failed. !!!"));
  errorFlag = ERR_NORAM;
  return false;
}

void Segment::deallocateData() {
  if (!data) { _dataLen = 0; return; }
  if ((Segment::getUsedSegmentData() > 0) && (_dataLen > 0)) { // check that we don't have a dangling / inconsistent data pointer
    //DEBUG_PRINTF_P(PSTR("---  Released data (%p): %d/%d -> %p\n"), this, _dataLen, Segment::getUsedSegmentData(), data);
    d_free(data);
  } else {
    DEBUG_PRINTF_P(PSTR("---- Released data (%p): inconsistent UsedSegmentData (%d/%d), cowardly refusing to free nothing.\n"), this, _dataLen, Segment::getUsedSegmentData());
  }
  data = nullptr;
  Segment::addUsedSegmentData(-_dataLen); // addUsedSegmentData guards agains negative and sets zero if underrun so this is safe
  _dataLen = 0;
}

/**
  * If reset of this segment was requested, clears runtime
  * settings of this segment.
  * Must not be called while an effect mode function is running
  * because it could access the data buffer and this method
  * may free that data buffer.
  */
void Segment::resetIfRequired() {
  if (!reset || !isActive()) return;
  //DEBUG_PRINTF_P(PSTR("-- Segment reset: %p\n"), this);
  if (data && _dataLen > 0) {
    if (_dataLen > FAIR_DATA_PER_SEG) deallocateData(); // do not keep large allocations
    else memset(data, 0, _dataLen);  // can prevent heap fragmentation
    DEBUG_PRINTF_P(PSTR("-- Segment %p reset, data cleared\n"), this);
  }
  if (pixels) for (size_t i = 0; i < length(); i++) pixels[i] = BLACK; // clear pixel buffer
  step = 0; call = 0; aux0 = 0; aux1 = 0;
  reset = false;
  #ifdef WLED_ENABLE_GIF
  endImagePlayback(this);
  #endif
}

void Segment::loadPalette(CRGBPalette16 &targetPalette, uint8_t pal) {
  // there is one randomly generated palette (1) followed by 4 palettes created from segment colors (2-5)
  // those are followed by 7 fastled palettes (6-12) and GRADIENT_PALETTE_COUNT built in gradient palettes
  // then come user custom palettes (IDs <=200) and usermod palettes (IDs 201-255), both growing downward from their respective base IDs
  // palette 0 is a varying palette depending on effect and may be replaced by segment's color if so
  // instructed in color_from_palette()
  if (pal == 0) pal = _default_palette; // _default_palette is set in setMode(), differs depending on effect
  const int umCount   = usermodPalettes.size();
  const int custCount = customPalettes.size();
  if (pal >= FIXED_PALETTE_COUNT) {
    if (pal > WLED_CUSTOM_PALETTE_ID_BASE) { // usermod range (IDs 201-255)
      if ((WLED_USERMOD_PALETTE_ID_BASE - pal) >= umCount) pal = 0;
    } else { // custom range
      if ((WLED_CUSTOM_PALETTE_ID_BASE - pal) >= custCount) pal = 0;
    }
  }
  switch (pal) {
    case 0: //default palette. Exceptions for specific effects above
      targetPalette = PartyColors_gc22;
      break;
    case 1: //randomly generated palette
      targetPalette = _randomPalette; //random palette is generated at intervals in handleRandomPalette()
      break;
    case 2: {//primary color only
      CRGB prim = colors[0];
      targetPalette = CRGBPalette16(prim);
      break;}
    case 3: {//primary + secondary
      CRGB prim = colors[0];
      CRGB sec  = colors[1];
      targetPalette = CRGBPalette16(prim,prim,sec,sec);
      break;}
    case 4: {//primary + secondary + tertiary
      CRGB prim = colors[0];
      CRGB sec  = colors[1];
      CRGB ter  = colors[2];
      targetPalette = CRGBPalette16(ter,sec,prim);
      break;}
    case 5: {//primary + secondary (+tertiary if not off), more distinct
      CRGB prim = colors[0];
      CRGB sec  = colors[1];
      if (colors[2]) {
        CRGB ter = colors[2];
        targetPalette = CRGBPalette16(prim,prim,prim,prim,prim,sec,sec,sec,sec,sec,ter,ter,ter,ter,ter,prim);
      } else {
        targetPalette = CRGBPalette16(prim,prim,prim,prim,prim,prim,prim,prim,sec,sec,sec,sec,sec,sec,sec,sec);
      }
      break;}
    default: //progmem palettes
      if (pal > WLED_CUSTOM_PALETTE_ID_BASE) { // usermod palette
        targetPalette = usermodPalettes[WLED_USERMOD_PALETTE_ID_BASE - pal].palette;
      } else if (pal >= FIXED_PALETTE_COUNT) { // user custom palette
        targetPalette = customPalettes[WLED_CUSTOM_PALETTE_ID_BASE - pal];
      } else if (pal < DYNAMIC_PALETTE_COUNT + FASTLED_PALETTE_COUNT) { // palette 6 - 12, fastled palettes
        targetPalette = *fastledPalettes[pal - DYNAMIC_PALETTE_COUNT];
      } else {
        byte tcp[72];
        memcpy_P(tcp, (byte*)pgm_read_dword(&(gGradientPalettes[pal - (DYNAMIC_PALETTE_COUNT + FASTLED_PALETTE_COUNT)])), sizeof(tcp));
        targetPalette.loadDynamicGradientPalette(tcp);
      }
      break;
  }
}

void Segment::handleTransition() {
  updateTransitionProgress();
  if (isInTransition() && !strip.isPoweringOff()) {
    // end transitions if completed but wait for a global power-off transition to complete to avoid revealing pixels (see blendSegment() blanking)
    const bool spatialDone = spatialProgress() == (_t->_flags & TRANSITION_FLAG_REVERSED ? 0 : 0xFFFFU);    
    if (_t->_oldSegment && spatialDone) {
      strip.invalidateStateCache();
      delete _t->_oldSegment; _t->_oldSegment = nullptr;
    }
    if (spatialDone && fadeProgress() == 0xFFFFU) {
      stopTransition(); // Transition frees a kept copy
    }
  }
}

/* Note on how transitions work:
  There are three transition channels: global on strip level that handles global brightness fading and triggering of segment spatial transitions (see led.cpp)
  on segment level there are two independent channels: a fade channel (_fadeProgress) that handles opacity/brightness & CCT (and colors/palette if FADE or as a fallback)
  and a spatial channel (_progress) that handles FX blending and swipe/push/etc. using a copy of the previous segment (aka oldSegment).
  FX transitions always need the oldSegment but can be spatial or fade and always use the spatial channel.
  There are many "special rules" that apply to handle transition updates i.e. calling startTransition() while a transition is already running.
  In general the transition logic was chosen to avoid glitches or flashing while allowing segments to act as individual "lights".
  Here is a short summary of the rules:
  - Segment opacity or global brightness always uses fade, they can run in parallel to any other transition (and even parallel to each other)
  - Off transition takes priority, in general no other transitions are allowed to simplify the logic, "offMode" is set once the global off finishes
    since this would require careful sync to spatial transition, the segment is held in transition until global finishes (see handleTransition() & blendSegment())
  - On strip level, there are flags to check for global on/off transitions which are set in toggleOnOff()
  - A global transition is started in stateUpdated() and triggers segment transitions if needed for spatial transitions
  - When a spatial on/off transition is triggered during an ongoing on/off transition, it is reversed by inverting the transition style (e.g. swipe-right becomes swipe-left)
  - Fade transitions continue from the current blend state if issued during a running transition
  - If a spatial transition is running it is never restarted. A subsequent change is deferred to the fade channel instead
  - For more details, see the comments throughout the code
*/

// startTransition() is called before changing a sement parameter, it captures the current state into _t and/or _t->_oldSegment and starts/updates transition timers.
// note: _t has the temporary "from" segment value(s) and the current segment holds the "to" values which are set after the transition starts.
//       the transition has two independent channels:
//       the fade channel (_fadeStart/_fadeDur/_fadeProgress) crossfades colors, palette, CCT and opacity and never needs a segment copy
//       the spatial channel (_spatialStart/_spatialDur/_progress and _oldSegment) renders wipe/push/etc. using a copy of the current state (oldSegment)
// kind: low nibble = TRANSITION_KIND_x identifying which change triggered the transition (determines whether a segment copy is needed)
//       high nibble = TRANSITION_POWER_x flags: POWER_ON/POWER_OFF = global on/off, POWER_TOGGLE = segment on/off (both flags are set)

void Segment::startTransition(uint16_t dur, uint8_t kind) {
  const uint8_t power = kind & TRANSITION_POWER_MASK;       // power flags (TRANSITION_POWER_*)
  kind &= TRANSITION_KIND_MASK;                             // strip the power flags
  const bool targetOn = power == TRANSITION_POWER_TOGGLE ? !on : power == TRANSITION_POWER_ON; // target on-state for power transitions
  // check if we even need to start a transition: abort if transitions disabled, not an active segment or not in an on state (unless this is a power-on request)
  if (dur == 0 || !isActive() || ((power != TRANSITION_POWER_TOGGLE) && !on)) {
    return;
  }
  // check if we need a copy of current segment: only effect transitions and transitions using a spatial (non-FADE) style
  const bool segmentCopy = kind == TRANSITION_KIND_EFFECT || (kind != TRANSITION_KIND_FADE && blendingStyle != TRANSITION_FADE);
  // helper lambda function to capture current _bri/_cct and optionally _colors to the segments transitions (_t) state  TDODO: needs refinement
  const auto captureBlend = [&](unsigned long fadeStart) {
    for (unsigned i = 0; i < NUM_COLORS; i++) _t->_colors[i] = color_blend16(_t->_colors[i], colors[i], _t->_fadeProgress);
    _t->_bri       = currentBri();
    _t->_cct       = currentCCT();
    _t->_prevPaletteBlends = 0;
    _t->_fadeDur   = dur;
    _t->_fadeStart = fadeStart;
  };

  // create a copy of the current segment to be used for spatial transitions (FX, palette, color, opacity, CCT)
  const auto createOldSegment = [&](uint16_t colorProgress) {
    if (_t->_oldSegment) { delete _t->_oldSegment; _t->_oldSegment = nullptr; }
    _t->_oldSegment = new(std::nothrow) Segment(*this); // store/copy current segment settings
    if (_t->_oldSegment) {
      for (unsigned i = 0; i < NUM_COLORS; i++) _t->_oldSegment->colors[i] = color_blend16(_t->_colors[i], colors[i], colorProgress);
      _t->_oldSegment->opacity = currentBri(); // capture current opacity in case it was being faded
      _t->_oldSegment->cct     = currentCCT(); // capture current CCT in case it was being faded
      if (!_t->_oldSegment->isActive()) { delete _t->_oldSegment; _t->_oldSegment = nullptr; } // pixel buffer allocation failed, use fallback
    }
    return _t->_oldSegment != nullptr;
  };

  if (isInTransition()) {
    // re-targeting a running transition: fade restarts, starting from current blend; a running spatial transition continues to completion
    if (!power) {
      // opacity/CCT/color/palette/FX change: rebase fades to the current visual blend and restart it (no jump). A running spatial transition continues
      if (segmentCopy && _t->_oldSegment == nullptr) {
        // no old segment means a fade transition is going on (color, palette, opacity, cct), capture current state into the old segment
        if (createOldSegment(_t->_fadeProgress)) {
          _t->_spatialStart = millis(); // start spatial transition (fading continues on current segment)
          _t->_spatialDur   = dur;
          DEBUGFX_PRINTF_P(PSTR("-- Updated transition with segment copy: S=%p T(%p) O[%p] OP[%p]\n"), this, _t, _t->_oldSegment, _t->_oldSegment->pixels);
        } else {
          // not enough RAM for segment copy: degrade to pure fade instead of dropping the transition
          captureBlend(millis()); // rebase fade channel to the current visual blend and restart it
        }
      }
      else if (_t->_spatialProgress > 0 || _t->_oldSegment == nullptr) {
        // note: if spatialProgress == 0, do not capture captureBlend(): for example an FX change also changes palette, we do not want that to fade in prallel
        if (!fadeTransitionActive() && _t->_oldSegment != nullptr) {
          // spatial transition with no fade running: enable fade and let the spatial transition continue. Need to capture the current "revealed" state i.e. copy segment colors to _t
          for (unsigned i = 0; i < NUM_COLORS; i++) _t->_colors[i] = colors[i]; // rebase transition colors&palette from current final state
          loadPalette(_t->_palT, palette);
        }
        captureBlend(millis()); // restart fade channel from the current visual state
        if (segmentCopy) {
          // if this is a deferred spatial request align fade duration with ongoing spatial channel
          const unsigned remainingProgress = (_t->_flags & TRANSITION_FLAG_REVERSED) ? _t->_spatialProgress : invertProgress(_t->_spatialProgress);
          _t->_fadeDur = ((uint32_t)_t->_spatialDur * remainingProgress) / 0xFFFFU;
        }
      }
      return;
    }
    // power transition (on or off) request
    if (_t->_flags & TRANSITION_FLAG_POWER) {
      // power transition request (per segment or global) during an ongoing power transition
      if (targetOn == ((_t->_flags & TRANSITION_FLAG_POWER_ON) != 0)) return; // same target re-issued, let the running transition finish
      if (blendingStyle != TRANSITION_FADE) {
        // already in a power transition reverse the animation: setting the REVERSED flag inverts the transition style
        // the timeline is flipped so it continues smoothly: e.g. 20%-on swipe-right becomes a 80% off swipe-left
        _t->_flags ^= TRANSITION_FLAG_REVERSED;
        unsigned prog  = (_t->_flags & TRANSITION_FLAG_REVERSED) ? invertProgress(_t->_spatialProgress) : _t->_spatialProgress; // "used time"
         _t->_spatialDur = dur;
        _t->_spatialStart = millis() - ((prog * dur) / 0xFFFFU);
        _t->_fadeDur = 0; // disable fading (any ongoing fade completes immediately)
        createOldSegment(0xFFFFU); // create a fresh copy from the final state which is currently displayed
      } else captureBlend(millis()); // capture current fade status and restart fade when toggling
      if (power == TRANSITION_POWER_TOGGLE) {
        // segment-level on/off
        if (_t->_oldSegment) {
          if (strip.isPoweringOff()) _t->_flags ^= TRANSITION_FLAG_POWER_ON; // flip POWER_ON flag, it is flipped back below, we need it to stay off if a segment is turned on during global off
          _t->_oldSegment->opacity = opacity;
          _t->_oldSegment->cct = cct;
        }
      }
      _t->_flags ^= TRANSITION_FLAG_POWER_ON; // flip POWER_ON flag
    } else {
      // global or segment on/off initiated: stop ongoing segment transition immediately, we do need the spatial channel and want to start a new transition
      if (_t->_oldSegment) { strip.invalidateStateCache(); delete _t->_oldSegment; _t->_oldSegment = nullptr; }
      captureBlend(millis()); // rebase transition values to current visual blend before starting the new power transition
      if (segmentCopy) {
        if (createOldSegment(0xFFFFU)) { // spatial transition, need a fresh copy (colors as-is, old side captures the current transition brightness)
          _t->_spatialStart = millis();
          _t->_spatialDur   = dur;
          _t->_fadeDur = 0; // non-fade power transition, do not fade anything but reveal the final state (same as a fresh power start)
          DEBUGFX_PRINTF_P(PSTR("-- Restarted power transition: S=%p T(%p) O[%p] OP[%p]\n"), this, _t, _t->_oldSegment, _t->_oldSegment->pixels);
        } else {
          // not enough RAM for segment copy: degrade to pure fade (restarted above) instead of dropping the transition
          _t->_spatialStart = 0; // disables the spatial channel and uses fade instead
        }
      } else {
        // FADE blending: the fade channel (restarted above) carries the power transition
        _t->_spatialStart = 0; // FADE blending: disables the spatial channel and uses fade instead
      }
      _t->_flags = TRANSITION_FLAG_POWER | (targetOn ? TRANSITION_FLAG_POWER_ON : 0);
    }
    return;
  }
  // no previous transition running, start by allocating memory for transition values
  _t = new(std::nothrow) Transition(dur);
  if (_t) {
    if (on) _t->_bri = opacity; // if segment is on, start from current opacity instead of the default 0 for proper opacity fade
    if (blendingStyle != TRANSITION_FADE && power) {
      _t->_fadeDur = 0; // if non-fade power transition, do not fade anything but reveal the final state
    }
    _t->_cct = cct;
    _t->_palette = palette;
    _t->_flags = power ? TRANSITION_FLAG_POWER | (targetOn ? TRANSITION_FLAG_POWER_ON : 0) : 0;
    loadPalette(_t->_palT, palette); // load target palette, will be blended in beginDraw() if FADE is used
    for (int i=0; i<NUM_COLORS; i++) _t->_colors[i] = colors[i];
    if (segmentCopy) createOldSegment(0xFFFFU); // spatial transition, create copy of current segment (falls back to fade if this fails)
    if (_t->_oldSegment) {
      DEBUGFX_PRINTF_P(PSTR("-- Started transition: S=%p T(%p) O[%p] OP[%p]\n"), this, _t, _t->_oldSegment, _t->_oldSegment->pixels);
    } else {
      _t->_spatialStart = 0; // disables the spatial channel and use fade i.e. enable fadeTransitionActive()
      DEBUGFX_PRINTF_P(PSTR("-- Started transition without old segment: S=%p T(%p)\n"), this, _t);
    }
  }
}

void Segment::stopTransition() {
  if (_t == nullptr) return; // no ongoing transition
  DEBUG_PRINTF_P(PSTR("-- Stopping transition: S=%p T(%p) O[%p]\n"), this, _t, _t->_oldSegment);
  strip.invalidateStateCache();
  delete _t;
  _t = nullptr;
}

// sets transition progress variables (0-65535) based on time passed since transition start
void Segment::updateTransitionProgress() const {
  if (isInTransition()) {
    _t->_spatialProgress = _t->_fadeProgress = 0xFFFFU;
    unsigned diff = millis() - _t->_spatialStart;
    if (_t->_spatialDur > 0 && diff < _t->_spatialDur) _t->_spatialProgress = diff * 0xFFFFU / _t->_spatialDur;
    if (_t->_flags & TRANSITION_FLAG_REVERSED) _t->_spatialProgress = invertProgress(_t->_spatialProgress);
    diff = millis() - _t->_fadeStart;
    if (_t->_fadeDur > 0 && diff < _t->_fadeDur) _t->_fadeProgress = diff * 0xFFFFU / _t->_fadeDur;
    // Fade transitions are never calculated reversed: instead we adjust the target and timing in startTranstion(.)
  }
}

// will return segment's CCT during a transition
// isPreviousMode() is actually not implemented for CCT in strip.service() as WLED does not support per-pixel CCT
uint8_t Segment::currentCCT() const {
  unsigned prog = fadeProgress();
  if (prog < 0xFFFFU) {
    // fade channel always crossfades CCT (never needs a segment copy)
    return (cct * prog + (_t->_cct * (0xFFFFU - prog))) / 0xFFFFU;
  }
  return cct;
}

// will return segment's opacity during a transition (blending it with old in case of FADE transition)
uint8_t Segment::currentBri() const {
  unsigned prog = fadeProgress();
  unsigned curBri = on ? opacity : 0;
  if (prog < 0xFFFFU) {
    curBri = (prog * curBri + _t->_bri * (0xFFFFU - prog)) / 0xFFFFU;
  }
  return curBri;
}

// pre-calculate drawing parameters for faster access (based on the idea from @softhack007 from MM fork)
// and blends colors and palettes if necessary
// prog is the progress of the fade channel (0-65535) and is passed to the function as it may be called in the context of old segment
// which does not have transition structure
void Segment::beginDraw(uint16_t prog) {
  setDrawDimensions();
  // load colors into _currentColors
  for (unsigned i = 0; i < NUM_COLORS; i++) _currentColors[i] = colors[i];
  // load palette into _currentPalette
  loadPalette(Segment::_currentPalette, palette);

  // color&palette fade blending: if using FADE or if changed during an ongoing spatial (swipe etc.) transition i.e. fadeTransitionActive()
  if (isInTransition() && prog < 0xFFFFU && fadeTransitionActive()) {
    // blend colors
    for (unsigned i = 0; i < NUM_COLORS; i++) _currentColors[i] = color_blend16(_t->_colors[i], colors[i], prog);
    // blend palettes
    // there are about 255 blend passes of 48 "blends" to completely blend two palettes (in _dur time)
    // minimum blend time is 100ms maximum is 65535ms
    unsigned noOfBlends = ((255U * prog) / 0xFFFFU) - _t->_prevPaletteBlends;
    if (noOfBlends > 255) noOfBlends = 255; // safety check
    for (unsigned i = 0; i < noOfBlends; i++, _t->_prevPaletteBlends++) nblendPaletteTowardPalette(_t->_palT, Segment::_currentPalette, 48);
    Segment::_currentPalette = _t->_palT; // copy transitioning/temporary palette
  }
}

// relies on WS2812FX::service() to call it for each frame
void Segment::handleRandomPalette() {
  unsigned long now = millis();
  uint16_t now_s = now / 1000; // we only need seconds (and @dedehai hated shift >> 10)
  now = (now_s)*1000 + (now % 1000); // ignore days (now is limited to 18 hours as now_s can only store 65535s ~ 18h 12min)
  if (now_s < Segment::_lastPaletteChange) Segment::_lastPaletteChange = 0; // handle overflow (will cause 2*randomPaletteChangeTime glitch at most)
  // is it time to generate a new palette?
  if (now_s > Segment::_lastPaletteChange + randomPaletteChangeTime) {
    Segment::_newRandomPalette  = useHarmonicRandomPalette ? generateHarmonicRandomPalette(Segment::_randomPalette) : generateRandomPalette();
    Segment::_lastPaletteChange = now_s;
    Segment::_nextPaletteBlend  = now; // starts blending immediately
  }
  // there are about 255 blend passes of 48 "blends" to completely blend two palettes (in strip.getTransition() time)
  // if randomPaletteChangeTime is shorter than strip.getTransition() palette will never fully blend
  unsigned frameTime = strip.getFrameTime();  // in ms [8-1000]
  unsigned transitionTime = strip.getTransition(); // in ms [100-65535]
  if ((uint16_t)now < Segment::_nextPaletteBlend || now > ((Segment::_lastPaletteChange*1000) + transitionTime + 2*frameTime)) return; // not yet time or past transition time, no need to blend
  unsigned transitionFrames = frameTime > transitionTime ? 1 : transitionTime / frameTime; // i.e. 700ms/23ms = 30 or 20000ms/8ms = 2500 or 100ms/1000ms = 0 -> 1
  unsigned noOfBlends = transitionFrames > 255 ? 1 : (255 + (transitionFrames>>1)) / transitionFrames;  // we do some rounding here
  for (unsigned i = 0; i < noOfBlends; i++) nblendPaletteTowardPalette(Segment::_randomPalette, Segment::_newRandomPalette, 48);
  Segment::_nextPaletteBlend = now + ((transitionFrames >> 8) * frameTime); // postpone next blend if necessary
}

// sets Segment geometry (length or width/height and grouping, spacing and offset as well as 2D mapping)
// strip must be suspended (strip.suspend()) before calling this function
// this function may call fill() to clear pixels if spacing or mapping changed (which requires setting _vWidth, _vHeight, _vLength or beginDraw())
void Segment::setGeometry(uint16_t i1, uint16_t i2, uint8_t grp, uint8_t spc, uint16_t ofs, uint16_t i1Y, uint16_t i2Y, uint8_t m12) {
  // Sanitise inputs
  if (i2 <= i1) { // For any values, this means deactivate the segment; we check i2 before i1 for this case
    i2 = 0;
  } 

  // If i1 is invalid, use old value
  // Valid range is inside maxWidth, or in trailing segment range
  if ((i1 >= Segment::maxWidth) && (i1 < Segment::maxWidth*Segment::maxHeight || i1 >= strip.getLengthTotal())) {
    i1 = start;
  }

  // Check i2 validity
  if (i2 > 0) {
    // Clamp i2 to maximum length
    if ((i1 >= Segment::maxWidth*Segment::maxHeight) && (i2 >= Segment::maxWidth*Segment::maxHeight)) {
      // Trailing strip after 2D
      i2 = MIN(i2,strip.getLengthTotal());
      i1Y = 0;  // 2D Y values are not used for trailing strip
      i2Y = 1;
    } else if (i2 > Segment::maxWidth) {
      i2 = Segment::maxWidth;
    }
  }

  #ifndef WLED_DISABLE_2D
  if (Segment::maxHeight>1) { // 2D
    if (i1Y >= Segment::maxHeight) {
      // Unlike i1 (X), Y values don't inherit old values if invalid
      // This behaviour preserved for backwards compatibility
      i1Y = 0;   
    }
    if (i2Y > Segment::maxHeight) {
      i2Y = Segment::maxHeight;
    } else if (i2Y < 1) {
      i2Y = 1;
    }
  } else
  #endif
  { 
    i1Y = 0;
    i2Y = 1;
  }

  if (grp == 0) { grp = 1; spc = 0; }  // prevent assignment of 0
  if (ofs == UINT16_MAX) ofs = offset; // keep current setting if passed illegal value
  m12 = constrain(m12, 0, 7);

  // Final safety check after all bounds adjustments
  if ((i1 >= i2) || (i1Y >= i2Y)) { 
    i2 = 0;  // disable segment
  }

  // Inputs are ok, check if anything has changed
  bool boundsUnchanged = (start == i1 && stop == i2)
  #ifndef WLED_DISABLE_2D
                       && ((Segment::maxHeight <= 1) || (startY == i1Y && stopY == i2Y))
  #endif
                       && (grouping == grp)
                       && (spacing == spc)
                       && (offset == ofs)
                       && (m12 == map1D2D);

  if (boundsUnchanged) return;

  DEBUG_PRINTF_P(PSTR("Segment geometry: (%d,%d),(%d,%d) -> (%d,%d),(%d,%d) [%d,%d]\n"), start, stop, startY, stopY, (int)i1, (int)i2, (int)i1Y, (int)i2Y, (int) grp, (int)spc);
  
  unsigned oldLength = length();

  strip.invalidateStateCache();
  markForReset();
  stopTransition(); // we can't use transition if segment dimensions changed
  stateChanged = true;      // send UDP/WS broadcast

  // apply change
  start = i1;
  stop = i2;
  startY = i1Y;
  stopY = i2Y;
  grouping = grp;
  spacing = spc;
  offset = ofs;
  map1D2D = m12;

  // Cleanup check
  auto newLength = length();
  if ((newLength > 0) && (newLength != oldLength)) {
    // allocate render buffer (always entire segment), prefer IRAM/PSRAM. Note: impact on FPS with PSRAM buffer is low (<2% with QSPI PSRAM) on S2/S3
    // Note we don't pass BFRALLOC_CLEAR as resetIfRequired() will initialize the buffer later
    p_free(pixels);
    pixels = static_cast<uint32_t*>(allocate_buffer(length() * sizeof(uint32_t), BFRALLOC_PREFER_PSRAM | BFRALLOC_NOBYTEACCESS));
    if (!pixels) {
      DEBUGFX_PRINTLN(F("!!! Not enough RAM for pixel buffer !!!"));
      errorFlag = ERR_NORAM_PX;
      stop = 0; // will fall through into disable check below
    }
  }

  if (length() == 0) {
    #ifdef WLED_ENABLE_GIF
    endImagePlayback(this);
    #endif
    deallocateData();
    p_free(pixels);
    pixels = nullptr;
  } else {
    refreshLightCapabilities();
  }
}


Segment &Segment::setColor(uint8_t slot, uint32_t c) {
  if (slot >= NUM_COLORS || c == colors[slot]) return *this;
  if (!_isRGB && !_hasW) {
    if (slot == 0 && c == BLACK) return *this; // on/off segment cannot have primary color black
    if (slot == 1 && c != BLACK) return *this; // on/off segment cannot have secondary color non black
  }
  //DEBUG_PRINTF_P(PSTR("- Starting color transition: %d [0x%X]\n"), slot, c);
  startTransition(strip.getTransition(), TRANSITION_KIND_DEFAULT); // start transition prior to change
  colors[slot] = c;
  stateChanged = true; // send UDP/WS broadcast
  return *this;
}

Segment &Segment::setCCT(uint16_t k) {
  if (k > 255) { //kelvin value, convert to 0-255
    if (k < 1900)  k = 1900;
    if (k > 10091) k = 10091;
    k = (k - 1900) >> 5;
  }
  if (cct != k) {
    //DEBUG_PRINTF_P(PSTR("- Starting CCT transition: %d\n"), k);
    startTransition(strip.getTransition(), TRANSITION_KIND_FADE); // start transition prior to change (no need to copy segment)
    cct = k;
    stateChanged = true; // send UDP/WS broadcast
  }
  return *this;
}

Segment &Segment::setOpacity(uint8_t o) {
  if (opacity != o) {
    DEBUG_PRINTF_P(PSTR("- Starting opacity transition: %d\n"), o);
    startTransition(strip.getTransition(), TRANSITION_KIND_FADE); // opacity change always fades (no segment copy needed)
    opacity = o;
    stateChanged = true; // send UDP/WS broadcast
  }
  return *this;
}

Segment &Segment::setOption(uint8_t n, bool val) {
  bool prev = (options >> n) & 0x01;
  if (val == prev) return *this;
  //DEBUG_PRINTF_P(PSTR("- Starting option transition: %d\n"), n);
  if (n == SEG_OPTION_ON) startTransition(strip.getTransition(), TRANSITION_KIND_DEFAULT | TRANSITION_POWER_TOGGLE); // on/off toggled, start transition
  if (val) options |=   0x01 << n;
  else     options &= ~(0x01 << n);
  stateChanged = true; // send UDP/WS broadcast
  return *this;
}

Segment &Segment::setMode(uint8_t fx, bool loadDefaults) {
  // skip reserved
  while (fx < strip.getModeCount() && strncmp_P("RSVD", strip.getModeData(fx), 4) == 0) fx++;
  if (fx >= strip.getModeCount()) fx = 0; // set solid mode
  // if we have a valid mode & is not reserved
  if (fx != mode) {
    startTransition(strip.getTransition(), TRANSITION_KIND_EFFECT); // set effect transitions (always needs a segment copy for blending)
    mode = fx;
    int sOpt;
    // load default values from effect string
    if (loadDefaults) {
      sOpt = extractModeDefaults(fx, "sx");  speed     = (sOpt >= 0) ? sOpt : DEFAULT_SPEED;
      sOpt = extractModeDefaults(fx, "ix");  intensity = (sOpt >= 0) ? sOpt : DEFAULT_INTENSITY;
      sOpt = extractModeDefaults(fx, "c1");  custom1   = (sOpt >= 0) ? sOpt : DEFAULT_C1;
      sOpt = extractModeDefaults(fx, "c2");  custom2   = (sOpt >= 0) ? sOpt : DEFAULT_C2;
      sOpt = extractModeDefaults(fx, "c3");  custom3   = (sOpt >= 0) ? sOpt : DEFAULT_C3;
      sOpt = extractModeDefaults(fx, "o1");  check1    = (sOpt >= 0) ? (bool)sOpt : false;
      sOpt = extractModeDefaults(fx, "o2");  check2    = (sOpt >= 0) ? (bool)sOpt : false;
      sOpt = extractModeDefaults(fx, "o3");  check3    = (sOpt >= 0) ? (bool)sOpt : false;
      sOpt = extractModeDefaults(fx, "m12"); if (sOpt >= 0) map1D2D   = constrain(sOpt, 0, 7); else map1D2D = M12_Pixels;  // reset mapping if not defined (2D FX may not work)
      sOpt = extractModeDefaults(fx, "si");  if (sOpt >= 0) soundSim  = constrain(sOpt, 0, 3);
      sOpt = extractModeDefaults(fx, "rev"); if (sOpt >= 0) reverse   = (bool)sOpt;
      sOpt = extractModeDefaults(fx, "mi");  if (sOpt >= 0) mirror    = (bool)sOpt; // NOTE: setting this option is a risky business
      sOpt = extractModeDefaults(fx, "rY");  if (sOpt >= 0) reverse_y = (bool)sOpt;
      sOpt = extractModeDefaults(fx, "mY");  if (sOpt >= 0) mirror_y  = (bool)sOpt; // NOTE: setting this option is a risky business
    }
    sOpt = extractModeDefaults(fx, "pal"); // always extract 'pal' to set _default_palette
    if (sOpt >= 0 && loadDefaults) setPalette(sOpt);
    if (sOpt <= 0) sOpt = 6; // partycolors if zero or not set
    _default_palette = sOpt; // _default_palette is loaded into pal0 in loadPalette() (if selected)
    markForReset();
    stateChanged = true; // send UDP/WS broadcast
  }
  return *this;
}

Segment &Segment::setPalette(uint8_t pal) {
  if (pal >= FIXED_PALETTE_COUNT) {
    if (pal > WLED_CUSTOM_PALETTE_ID_BASE) { // usermod range
      if ((WLED_USERMOD_PALETTE_ID_BASE - pal) >= (int)usermodPalettes.size()) pal = 0;
    } else { // custom range
      if ((WLED_CUSTOM_PALETTE_ID_BASE - pal) >= (int)customPalettes.size()) pal = 0;
    }
  }
  if (pal != palette) {
    //DEBUG_PRINTF_P(PSTR("- Starting palette transition: %d\n"), pal);
    startTransition(strip.getTransition(), TRANSITION_KIND_DEFAULT); // start transition prior to change
    palette = pal;
    stateChanged = true; // send UDP/WS broadcast
  }
  return *this;
}

Segment &Segment::setName(const char *newName) {
  if (newName) {
    const int newLen = min(strlen(newName), (size_t)WLED_MAX_SEGNAME_LEN);
    if (newLen) {
      char *newBuf = static_cast<char*>(allocate_buffer(newLen+1, BFRALLOC_PREFER_PSRAM));
      if (newBuf) {
        strlcpy(newBuf, newName, newLen+1);
        if (mode == FX_MODE_2DSCROLLTEXT) startTransition(strip.getTransition(), TRANSITION_KIND_EFFECT); // if the name changes in scrolling text mode, we need to copy the segment for blending
        char *oldName = name;
        name = newBuf;
        if (oldName) p_free(oldName);
      }
      return *this;
    }
  }
  return clearName();
}

// 2D matrix
unsigned Segment::virtualWidth() const {
  unsigned groupLen = groupLength();
  unsigned vWidth = ((transpose ? height() : width()) + groupLen - 1) / groupLen;
  if (mirror) vWidth = (vWidth + 1) /2;  // divide by 2 if mirror, leave at least a single LED
  return vWidth;
}

unsigned Segment::virtualHeight() const {
  unsigned groupLen = groupLength();
  unsigned vHeight = ((transpose ? width() : height()) + groupLen - 1) / groupLen;
  if (mirror_y) vHeight = (vHeight + 1) /2;  // divide by 2 if mirror, leave at least a single LED
  return vHeight;
}

// Constants for mapping mode "Pinwheel"
#ifndef WLED_DISABLE_2D
constexpr int Fixed_Scale = 16384; // fixpoint scaling factor (14bit for fraction)
// Pinwheel helper function: matrix dimensions to number of rays
static int getPinwheelLength(int vW, int vH) {
  // Returns multiple of 8, prevents over drawing
  return (max(vW, vH) + 15) & ~7;
}
static void setPinwheelParameters(int i, int vW, int vH, int& startx, int& starty, int* cosVal, int* sinVal, bool getPixel = false) {
  int steps = getPinwheelLength(vW, vH);
  int baseAngle = ((0xFFFF + steps / 2) / steps);  // 360° / steps, in 16 bit scale round to nearest integer
  int rotate = 0;
  if (getPixel) rotate = baseAngle / 2; // rotate by half a ray width when reading pixel color
  for (int k = 0; k < 2; k++) // angular steps for two consecutive rays
  {
    int angle = (i + k) * baseAngle + rotate;
    cosVal[k] = (cos16_t(angle) * Fixed_Scale) >> 15; // step per pixel in fixed point, cos16 output is -0x7FFF to +0x7FFF
    sinVal[k] = (sin16_t(angle) * Fixed_Scale) >> 15; // using explicit bit shifts as dividing negative numbers is not equivalent (rounding error is acceptable)
  }
  startx = (vW * Fixed_Scale) / 2; // + cosVal[0] / 4; // starting position = center + 1/4 pixel (in fixed point)
  starty = (vH * Fixed_Scale) / 2; // + sinVal[0] / 4;
}
#endif

// 1D strip
uint16_t Segment::virtualLength() const {
#ifndef WLED_DISABLE_2D
  if (is2D()) {
    unsigned vW = virtualWidth();
    unsigned vH = virtualHeight();
    unsigned vLen;
    switch (map1D2D) {
      case M12_pBar:
        vLen = vH;
        break;
      case M12_pCorner:
        vLen = max(vW,vH); // get the longest dimension
        break;
      case M12_pArc:
        vLen = (sqrt32_bw((vH*vH + vW*vW) << 2) + 1) >> 1; // diagonal rounded
        break;
      case M12_sPinwheel:
        vLen = getPinwheelLength(vW, vH);
        break;
      default:
        vLen = vW * vH; // use all pixels from segment
        break;
    }
    return vLen;
  }
#endif
  unsigned groupLen = groupLength(); // is always >= 1
  unsigned vLength = (length() + groupLen - 1) / groupLen;
  if (mirror) vLength = (vLength + 1) /2;  // divide by 2 if mirror, leave at least a single LED
  return vLength;
}

#ifndef WLED_DISABLE_2D
// maximum length of a mapped 1D segment, used in PS for buffer allocation
uint16_t Segment::maxMappingLength() const {
  uint32_t vW = virtualWidth();
  uint32_t vH = virtualHeight();
  return max(sqrt32_bw(vH*vH + vW*vW), (uint32_t)getPinwheelLength(vW, vH)); // use diagonal
}
#endif
// pixel is clipped if it falls outside clipping range
// if clipping start > stop the clipping range is inverted
bool Segment::isPixelClipped(int i, uint8_t style) const {
  if (style != TRANSITION_FADE && isInTransition() && _clipStart != _clipStop) {
    bool invert = _clipStart > _clipStop;  // ineverted start & stop
    int start = invert ? _clipStop : _clipStart;
    int stop  = invert ? _clipStart : _clipStop;
    if (style == TRANSITION_FAIRY_DUST) {
      unsigned len = stop - start;
      if (len < 2) return false;
      unsigned shuffled = hashInt(i) % len;
      unsigned pos = (shuffled * 0xFFFFU) / len;
      return (spatialProgress() <= pos);
    }
    const bool iInside = (i >= start && i < stop);
    bool isClipped = !iInside ^ invert; // thanks @willmmiles (https://github.com/wled/WLED/pull/3877#discussion_r1554633876)
    return isClipped;
  }
  return false;
}

void WLED_O2_ATTR Segment::setPixelColor(int i, uint32_t col) const
{
  if (!isActive() || i < 0) return; // not active or invalid index
#ifndef WLED_DISABLE_2D
  int vStrip = 0;
#endif
  const int vL = vLength();
  // if the 1D effect is using virtual strips "i" will have virtual strip id stored in upper 16 bits
  // in such case "i" will be > virtualLength()
  if (i >= vL) {
    // check if this is a virtual strip
    #ifndef WLED_DISABLE_2D
    vStrip = i>>16; // hack to allow running on virtual strips (2D segment columns/rows)
    #endif
    i &= 0xFFFF;          // truncate vstrip index. note: vStrip index is 1 even in 1D, still need to truncate
    if (i >= vL) return;  // if pixel would still fall out of segment just exit
  }

#ifndef WLED_DISABLE_2D
  if (is2D()) {
    const int vW = vWidth();   // segment width in logical pixels (can be 0 if segment is inactive)
    const int vH = vHeight();  // segment height in logical pixels (is always >= 1)
    const auto XY = [&](unsigned x, unsigned y){ return x + y*vW;};
    switch (map1D2D) {
      case M12_Pixels:
        // use all available pixels as a long strip
        setPixelColorRaw(XY(i % vW, i / vW), col);
        break;
      case M12_pBar:
        // expand 1D effect vertically or have it play on virtual strips
        if (vStrip > 0)                   setPixelColorRaw(XY(vStrip - 1, vH - i - 1), col);
        else for (int x = 0; x < vW; x++) setPixelColorRaw(XY(x, vH - i - 1), col);
        break;
      case M12_pArc: {
        // expand in circular fashion from top left corner
        // Tony Barrera's circle algorithm
        // https://softwareengineering.stackexchange.com/questions/287478/drawing-concentric-circles-without-gaps/357445#357445 
        
        if      (i == 0)           setPixelColorXYRaw(0, 0, col); // 0 and 2 special cases to prevent square
        else if (i == 2)           setPixelColorXYRaw(1, 1, col);
        else if (i == vLength()-1) setPixelColorXYRaw(vW-1, vH-1, col); // last i always draws bottom right corner

        int x = 0, y = i;  // i is the radius
        int d = -(i >> 1); // Initial decision parameter

        while (x <= y) {
          if (i != x || i != y) { // prevent early square
            if (y < vH && x < vW) setPixelColorXYRaw(x, y, col);
            if (y < vW && x < vH) setPixelColorXYRaw(y, x, col);
          }
          if (d <= 0) d += ++x;
          else d -= --y;
        }
        break;
      }
      case M12_pCorner:
        for (int x = 0; x <= i; x++) setPixelColorXY(x, i, col); // note: <= to include i=0. Relies on overflow check in sPC()
        for (int y = 0; y <  i; y++) setPixelColorXY(i, y, col);
        break;
      case M12_sPinwheel: {
        // Uses Bresenham's algorithm to place coordinates of two lines in arrays then draws between them
        int startX, startY, cosVal[2], sinVal[2]; // in fixed point scale
        setPinwheelParameters(i, vW, vH, startX, startY, cosVal, sinVal);

        unsigned maxLineLength = max(vW, vH) + 2; // pixels drawn is always smaller than dx or dy, +1 pair for rounding errors
        uint16_t lineCoords[2][maxLineLength];    // uint16_t to save ram
        int lineLength[2] = {0};

        static int prevRays[2] = {INT_MAX, INT_MAX}; // previous two ray numbers
        int closestEdgeIdx = INT_MAX; // index of the closest edge pixel

        for (int lineNr = 0; lineNr < 2; lineNr++) {
          int x0 = startX; // x, y coordinates in fixed scale
          int y0 = startY;
          int x1 = (startX + (cosVal[lineNr] << 9)); // outside of grid
          int y1 = (startY + (sinVal[lineNr] << 9)); // outside of grid
          const int dx =  abs(x1-x0), sx = x0<x1 ? 1 : -1; // x distance & step
          const int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1; // y distance & step
          uint16_t* coordinates = lineCoords[lineNr]; // 1D access is faster
          int* length = &lineLength[lineNr];          // faster access
          x0 /= Fixed_Scale; // convert to pixel coordinates
          y0 /= Fixed_Scale;

          // Bresenham's algorithm
          int idx = 0;
          int err = dx + dy;
          while (true) {
            if ((unsigned)x0 >= (unsigned)vW || (unsigned)y0 >= (unsigned)vH) {
              closestEdgeIdx = min(closestEdgeIdx, idx-2);
              break; // stop if outside of grid (exploit unsigned int overflow)
            }
            coordinates[idx++] = x0;
            coordinates[idx++] = y0;
            (*length)++;
            // note: since endpoint is out of grid, no need to check if endpoint is reached
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
          }
        }

        // fill up the shorter line with missing coordinates, so block filling works correctly and efficiently
        int diff = lineLength[0] - lineLength[1];
        int longLineIdx = (diff > 0) ? 0 : 1;
        int shortLineIdx = longLineIdx ? 0 : 1;
        if (diff != 0) {
          int idx = (lineLength[shortLineIdx] - 1) * 2; // last valid coordinate index
          int lastX = lineCoords[shortLineIdx][idx++];
          int lastY = lineCoords[shortLineIdx][idx++];
          bool keepX = lastX == 0 || lastX == vW - 1;
          for (int d = 0; d < abs(diff); d++) {
            lineCoords[shortLineIdx][idx] = keepX ? lastX :lineCoords[longLineIdx][idx];
            idx++;
            lineCoords[shortLineIdx][idx] =  keepX ? lineCoords[longLineIdx][idx] : lastY;
            idx++;
          }
        }

        // draw and block-fill the line coordinates. Note: block filling only efficient if angle between lines is small
        closestEdgeIdx += 2;
        int max_i = getPinwheelLength(vW, vH) - 1;
        bool drawFirst = !(prevRays[0] == i - 1 || (i == 0 && prevRays[0] == max_i)); // draw first line if previous ray was not adjacent including wrap
        bool drawLast  = !(prevRays[0] == i + 1 || (i == max_i && prevRays[0] == 0)); // same as above for last line
        for (int idx = 0; idx < lineLength[longLineIdx] * 2;) { //!! should be long line idx!
          int x1 = lineCoords[0][idx];
          int x2 = lineCoords[1][idx++];
          int y1 = lineCoords[0][idx];
          int y2 = lineCoords[1][idx++];
          int minX, maxX, minY, maxY;
          (x1 < x2) ? (minX = x1, maxX = x2) : (minX = x2, maxX = x1);
          (y1 < y2) ? (minY = y1, maxY = y2) : (minY = y2, maxY = y1);

          // fill the block between the two x,y points
          bool alwaysDraw = (drawFirst && drawLast) || // No adjacent rays, draw all pixels
                            (idx > closestEdgeIdx)  || // Edge pixels on uneven lines are always drawn
                            (i == 0 && idx == 2)    || // Center pixel special case
                            (i == prevRays[1]);        // Effect drawing twice in 1 frame
          for (int x = minX; x <= maxX; x++) {
            for (int y = minY; y <= maxY; y++) {
              bool onLine1 = x == x1 && y == y1;
              bool onLine2 = x == x2 && y == y2;
              if ((alwaysDraw) ||
                  (!onLine1 && (!onLine2 || drawLast))  || // Middle pixels and line2 if drawLast
                  (!onLine2 && (!onLine1 || drawFirst))    // Middle pixels and line1 if drawFirst
                ) {
                setPixelColorXY(x, y, col);
              }
            }
          }
        }
        prevRays[1] = prevRays[0];
        prevRays[0] = i;
        break;
      }
    }
    return;
  } else if (Segment::maxHeight != 1 && (width() == 1 || height() == 1)) {
    if (start < Segment::maxWidth*Segment::maxHeight) {
      // we have a vertical or horizontal 1D segment (WARNING: virtual...() may be transposed)
      int x = 0, y = 0;
      if (vHeight() > 1) y = i;
      if (vWidth()  > 1) x = i;
      setPixelColorXY(x, y, col);
      return;
    }
  }
#endif
  setPixelColorRaw(i, col);
}

#ifdef WLED_USE_AA_PIXELS
// anti-aliased normalized version of setPixelColor()
void Segment::setPixelColor(float i, uint32_t col, bool aa) const
{
  if (!isActive()) return; // not active
  int vStrip = int(i/10.0f); // hack to allow running on virtual strips (2D segment columns/rows)
  i -= int(i);

  if (i<0.0f || i>1.0f) return; // not normalized

  float fC = i * (virtualLength()-1);
  if (aa) {
    unsigned iL = roundf(fC-0.49f);
    unsigned iR = roundf(fC+0.49f);
    float    dL = (fC - iL)*(fC - iL);
    float    dR = (iR - fC)*(iR - fC);
    uint32_t cIL = getPixelColor(iL | (vStrip<<16));
    uint32_t cIR = getPixelColor(iR | (vStrip<<16));
    if (iR!=iL) {
      // blend L pixel
      cIL = color_blend(col, cIL, uint8_t(dL*255.0f));
      setPixelColor(iL | (vStrip<<16), cIL);
      // blend R pixel
      cIR = color_blend(col, cIR, uint8_t(dR*255.0f));
      setPixelColor(iR | (vStrip<<16), cIR);
    } else {
      // exact match (x & y land on a pixel)
      setPixelColor(iL | (vStrip<<16), col);
    }
  } else {
    setPixelColor(int(roundf(fC)) | (vStrip<<16), col);
  }
}
#endif

uint32_t WLED_O2_ATTR Segment::getPixelColor(int i) const
{
  if (!isActive() || i < 0) return 0; // not active or invalid index

#ifndef WLED_DISABLE_2D
  int vStrip = i>>16; // virtual strips are only relevant in Bar expansion mode
  i &= 0xFFFF;
#endif
  if (i >= (int)vLength()) return 0;

#ifndef WLED_DISABLE_2D
  if (is2D()) {
    const int vW = vWidth();   // segment width in logical pixels (can be 0 if segment is inactive)
    const int vH = vHeight();  // segment height in logical pixels (is always >= 1)
    int x = 0, y = 0;
    switch (map1D2D) {
      case M12_Pixels:
        x = i % vW;
        y = i / vW;
        break;
      case M12_pBar:
        if (vStrip > 0) { x = vStrip - 1; y = vH - i - 1; }
        else            { y = vH - i - 1; };
        break;
      case M12_pArc:
        if (i >= vW && i >= vH) { // Barrera's circle algorithm
          x = vW - 1; y = vH - 1; // default to bottom right if not found
          int x2 = 0, y2 = i, d = -(i >> 1);
          int validCount = 0; // return 2nd non mirrored pixel if available
          while (x2 <= y2 && validCount < 2) {
            if      (y2 < vH && x2 < vW) {x = x2, y = y2; validCount++;}
            else if (y2 < vW && x2 < vH) {x = y2, y = x2; validCount++;}

            if (d <= 0) d += ++x2; else d -= --y2;
          }
          break;
        }
        // otherwise fallthrough
      case M12_pCorner:
        // use longest dimension
        if (vW > vH) x = i;
        else         y = i;
        break;
      case M12_sPinwheel: {
        // not 100% accurate, returns pixel at outer edge
        int cosVal[2], sinVal[2];
        setPinwheelParameters(i, vW, vH, x, y, cosVal, sinVal, true);
        int maxX = (vW-1) * Fixed_Scale;
        int maxY = (vH-1) * Fixed_Scale;
        // trace ray from center until we hit any edge - to avoid rounding problems, we use fixed point coordinates
        while ((x < maxX)  && (y < maxY) && (x > Fixed_Scale) && (y > Fixed_Scale)) {
          x += cosVal[0]; // advance to next position
          y += sinVal[0];
        }
        x /= Fixed_Scale;
        y /= Fixed_Scale;
        break;
      }
    }
    return getPixelColorXY(x, y);
  }
#endif
  return getPixelColorRaw(i);
}

void Segment::refreshLightCapabilities() const {
  unsigned capabilities = 0;

  if (!isActive()) {
    _capabilities = 0;
    return;
  }

  // Traverse physical pixels once (avoids needing a permanent forward mapping table)
  bool loadedTempInv = false;
  if ((strip._hasCustomFileMap || strip._hasPanelGaps) && !strip._invMappingTable && !strip.customMappingTable) {
    if (strip._hasCustomFileMap) loadedTempInv = strip.loadInvMapFromFile(currentLedmap);
    else if (strip.isMatrix) { strip.setUpMatrix(); loadedTempInv = (strip._invMappingTable != nullptr); }
  }
  const uint16_t physLen = strip.getLength();
  const bool is2DSeg = strip.isMatrix && ((size_t)start + (size_t)startY * Segment::maxWidth + length() <= (size_t)Segment::maxWidth * Segment::maxHeight);
  for (uint16_t phys = 0; phys < physLen; phys++) {
    uint16_t logIdx = strip.findLogicalPixel(phys);
    if (logIdx == 0xFFFFU) continue;
    if (is2DSeg) {
      const uint16_t x = logIdx % Segment::maxWidth;
      const uint16_t y = logIdx / Segment::maxWidth;
      if (x < start || x >= stop || y < startY || y >= stopY) continue;
    } else {
      if (logIdx < start || logIdx >= stop) continue;
    }
    unsigned index = phys;
    for (unsigned b = 0; b < BusManager::getNumBusses(); b++) {
      const Bus *bus = BusManager::getBus(b);
      if (!bus || !bus->isOk()) break;
      if (bus->containsPixel(index)) {
        if (bus->hasRGB() || (strip.cctFromRgb && bus->hasCCT())) capabilities |= SEG_CAPABILITY_RGB;
        if (!strip.cctFromRgb && bus->hasCCT())                   capabilities |= SEG_CAPABILITY_CCT;
        if (strip.correctWB && (bus->hasRGB() || bus->hasCCT()))  capabilities |= SEG_CAPABILITY_CCT; //white balance correction (CCT slider)
        if (bus->hasWhite()) {
          unsigned aWM = Bus::getGlobalAWMode() == AW_GLOBAL_DISABLED ? bus->getAutoWhiteMode() : Bus::getGlobalAWMode();
          bool whiteSlider = (aWM == RGBW_MODE_DUAL || aWM == RGBW_MODE_MANUAL_ONLY); // white slider allowed
          // if auto white calculation from RGB is active (Accurate/Brighter), force RGB controls even if there are no RGB busses
          if (!whiteSlider) capabilities |= SEG_CAPABILITY_RGB;
          // if auto white calculation from RGB is disabled/optional (None/Dual), allow white channel adjustments
          if ( whiteSlider) capabilities |= SEG_CAPABILITY_W;
        }
        break;
      }
    }
  }
  if (loadedTempInv) {
    d_free(strip._invMappingTable);
    strip._invMappingTable = nullptr;
    strip._invMappingSize = 0;
  }
  _capabilities = capabilities;
}

/*
 * Fills segment with color
 */
void Segment::fill(uint32_t c) const {
  if (!isActive()) return; // not active
  for (unsigned i = 0; i < length(); i++) setPixelColorRaw(i,c); // always fill all pixels (blending will take care of grouping, spacing and clipping)
}

/*
 * fade out function, higher rate = quicker fade
 * fading is highly dependant on frame rate (higher frame rates, faster fading)
 * each frame will fade at max 9% or as little as 0.8%
 */
void Segment::fade_out(uint8_t rate) const {
  if (!isActive()) return; // not active
  rate = (256-rate) >> 1;
  const int mappedRate = 256 / (rate + 1);
  const size_t rlength = rawLength();  // calculate only once
  for (unsigned j = 0; j < rlength; j++) {
    uint32_t color = getPixelColorRaw(j);
    if (color == colors[1]) continue; // already at target color
    for (int i = 0; i < 32; i += 8) {
      uint8_t c2 = (colors[1]>>i);  // get background channel
      uint8_t c1 = (color>>i);      // get foreground channel
      // we can't use bitshift since we are using int
      int delta = (c2 - c1) * mappedRate / 256;
      // if fade isn't complete, make sure delta is at least 1 (fixes rounding issues)
      if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1) ? 1 : -1;
      // stuff new value back into color
      color &= ~(0xFF<<i);
      color |= ((c1 + delta) & 0xFF) << i;
    }
    setPixelColorRaw(j, color);
  }
}

// fades all pixels to secondary color
void Segment::fadeToSecondaryBy(uint8_t fadeBy) const {
  if (!isActive() || fadeBy == 0) return;   // optimization - no scaling to apply
  const size_t rlength = rawLength();  // calculate only once
  for (unsigned i = 0; i < rlength; i++) setPixelColorRaw(i, color_blend(getPixelColorRaw(i), colors[1], fadeBy));
}

// fades all pixels to black using nscale8()
void Segment::fadeToBlackBy(uint8_t fadeBy) const {
  if (!isActive() || fadeBy == 0) return;   // optimization - no scaling to apply
  const size_t rlength = rawLength();  // calculate only once
  for (unsigned i = 0; i < rlength; i++) setPixelColorRaw(i, fast_color_scale(getPixelColorRaw(i), 255-fadeBy));
}

/*
 * blurs segment content, source: FastLED colorutils.cpp
 * Note: for blur_amount > 215 this function does not work properly (creates alternating pattern)
 */
void Segment::blur(uint8_t blur_amount, bool smear) const {
  if (!isActive() || blur_amount == 0) return; // optimization: 0 means "don't blur"
#ifndef WLED_DISABLE_2D
  if (is2D()) {
    // compatibility with 2D
    blur2D(blur_amount, blur_amount, smear); // symmetrical 2D blur
    //box_blur(map(blur_amount,1,255,1,3), smear);
    return;
  }
#endif
  uint8_t keep = smear ? 255 : 255 - blur_amount;
  uint8_t seep = blur_amount >> 1;
  unsigned vlength = vLength();
  // handle first pixel to avoid conditional in loop (faster)
  uint32_t cur = getPixelColorRaw(0);
  uint32_t carryover = fast_color_scale(cur, seep);
  setPixelColorRaw(0, fast_color_scale(cur, keep));
  for (unsigned i = 1; i < vlength; i++) {
    cur = getPixelColorRaw(i);
    uint32_t part = fast_color_scale(cur, seep);
    cur = fast_color_scale(cur, keep);
    cur = color_add(cur, carryover);
    setPixelColorRaw(i - 1, color_add(getPixelColorRaw(i - 1), part)); // previous pixel
    setPixelColorRaw(i, cur); // current pixel
    carryover = part;
  }
}

/*
 * Put a value 0 to 255 in to get a color value.
 * The colours are a transition r -> g -> b -> back to r
 * Rotates the color in HSV space, where pos is H. (0=0deg, 256=360deg)
 */
uint32_t Segment::color_wheel(uint8_t pos) const {
  if (palette) return color_from_palette(pos, false, true, 0); // color_wheel is a continuous (moving) wheel, so wrap end->start (restores pre-0.16 behaviour)
  CRGBW rgb;
  rgb = CHSV32(static_cast<uint16_t>(pos << 8), 255, 255);
  rgb.w = W(getCurrentColor(0)); // add white channel
  return rgb.color32;
}

/*
 * Gets a single color from the currently selected palette.
 * @param i Palette Index (if mapping is true, the full palette will be _virtualSegmentLength long, if false, 255). Will wrap around automatically.
 * @param mapping if true, LED position in segment is considered for color
 * @param moving FastLED palettes will usually wrap back to the start smoothly. Set to true if effect has moving palette and you want wrap.
 * @param mcol If the default palette 0 is selected, return the standard color 0, 1 or 2 instead. If >2, Party palette is used instead
 * @param pbri Value to scale the brightness of the returned color by. Default is 255. (no scaling)
 * @returns Single color from palette
 */
uint32_t Segment::color_from_palette(uint16_t i, bool mapping, bool moving, uint8_t mcol, uint8_t pbri) const {
  uint32_t color = getCurrentColor(mcol);
  // default palette or no RGB support on segment
  if ((palette == 0 && mcol < NUM_COLORS) || !_isRGB) {
    return color_fade(color, pbri, true);
  }

  unsigned paletteIndex = i;
  if (mapping) paletteIndex = min((i*255)/vLength(), 255U);
  // paletteBlend: 0 - wrap when moving, 1 - always wrap, 2 - never wrap, 3 - none (undefined/no interpolation of palette entries)
  // ColorFromPalette interpolations are: NOBLEND, LINEARBLEND, LINEARBLEND_NOWRAP
  TBlendType blend = NOBLEND;
  switch (paletteBlend) {
    case 0: blend = moving ? LINEARBLEND : LINEARBLEND_NOWRAP; break;
    case 1: blend = LINEARBLEND; break;
    case 2: blend = LINEARBLEND_NOWRAP; break;
  }
  CRGBW palcol = ColorFromPalette(_currentPalette, paletteIndex, pbri, blend);
  palcol.w = W(color);

  return palcol.color32;
}


///////////////////////////////////////////////////////////////////////////////
// WS2812FX class implementation
///////////////////////////////////////////////////////////////////////////////

//do not call this method from system context (network callback)
void WS2812FX::finalizeInit() {
  _stateCacheValid = false;
  //reset segment runtimes
  restartRuntime();

  // for the lack of better place enumerate ledmaps here
  // if we do it in json.cpp (serializeInfo()) we are getting flashes on LEDs
  // unfortunately this means we do not get updates after uploads
  // the other option is saving UI settings which will cause enumeration
  enumerateLedmaps();

  _hasWhiteChannel = _isOffRefreshRequired = false;
  BusManager::removeAll();
  // TODO: ideally we would free everything segment related here to reduce fragmentation (pixel buffers, ledamp, segments, etc) but that somehow leads to heap corruption if touchig any of the buffers.
  // First assign driver types (RMT/I2S/... channels) for all buses before any are constructed. If hardware allocation fails
  // (no free channels, no fallback) the bus is still constructed below and might fail construction.
  // BusManager::add() replaces failed buses with a BusPlaceholder, edge cases of bad configs can slip through.
  #if defined(ARDUINO_ARCH_ESP32)
  unsigned rmtBusCount = 0;
  unsigned parHwBusCount = 0; // used for debug print only
  #endif
  for (auto &bus : busConfigs) {
    if (!Bus::isDigital(bus.type)) continue;
    if (!BusManager::allocateHardware(bus.type, bus.pins, bus.driverType)) {
      DEBUG_PRINTLN(F("Bus driver allocation failed."));
      continue; // TODO: avoid constructing this bus when no driver could be allocated?
    }
    #if defined(ARDUINO_ARCH_ESP32)
    // Count buses that will occupy an RMT channel (driverType is final after allocateHardware())
    if (!Bus::is2Pin(bus.type)) {
      if (bus.driverType == BUSDRV_RMT) rmtBusCount++;
      else if (bus.driverType == BUSDRV_PARHW) parHwBusCount++; // parallel bus: I2S/LCD/SPI/PARLIO depending on chip (hardware driven, not BB)
    }
    #endif
  }

  #if defined(ARDUINO_ARCH_ESP32)
  DEBUG_PRINTF_P(PSTR("Digital RMT buses: %u, parallel buses: %u\n"), rmtBusCount, parHwBusCount);
  // RMT channel tracking was already reset by BusManager::removeAll() -> resetChannelTracking()
  BusManager::setExpectedRmtChannelCount((uint8_t)rmtBusCount);
  #endif

  DEBUG_PRINTF_P(PSTR("Heap before buses: %d\n"), getFreeHeapSize());
  // Now construct each bus. BusManager::add() automatically falls back to a BusPlaceholder if it fails
  for (auto &bus : busConfigs) {
    BusManager::add(bus, false);
  }
  busConfigs.clear();
  busConfigs.shrink_to_fit();

  _length = 0;
  for (size_t i=0; i<BusManager::getNumBusses(); i++) {
    Bus *bus = BusManager::getBus(i);
    if (!bus || bus->getStart() + bus->getLength() > MAX_LEDS) break;
    if (!bus->isOk()) continue; // placeholder bus (failed init) — skip but keep initializing remaining buses
    // This must be done after all buses have been created, as parallel buses interact.
    bus->begin();
    if (!bus->isOk()) {
      DEBUG_PRINTF_P(PSTR("Bus %u failed hardware initialization.\n"), (unsigned)i);
      continue; // TODO: preserve the original configuration when late initialization fails?
    }
    //RGBW mode is enabled if at least one of the strips is RGBW
    _hasWhiteChannel |= bus->hasWhite();
    //refresh is required to remain off if at least one of the strips requires the refresh.
    _isOffRefreshRequired |= bus->isOffRefreshRequired() && !bus->isPWM(); // use refresh bit for phase shift with analog
    unsigned busEnd = bus->getStart() + bus->getLength();
    if (busEnd > _length) _length = busEnd;
    bus->setBrightness16(scaledBri16(bri<<8));
  }
  BusManager::initializeABL(); // init brightness limiter
  DEBUG_PRINTF_P(PSTR("Heap after buses: %d\n"), ESP.getFreeHeap());

  Segment::maxWidth  = _length;
  Segment::maxHeight = 1;

  //segments are created in makeAutoSegments();
  DEBUG_PRINTLN(F("Loading custom palettes"));
  loadCustomPalettes(); // (re)load all custom palettes
  DEBUG_PRINTLN(F("Loading custom ledmaps"));
  deserializeMap();     // (re)load default ledmap (will also setUpMatrix() if ledmap does not exist)

  // allocate frame buffer after matrix has been set up (gaps!)
  updatePixelBuffer();
  DEBUG_PRINTF_P(PSTR("Heap after strip init: %uB\n"), getFreeHeapSize());
}

void WS2812FX::updatePixelBuffer() {
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
  p_free(_physOwner);
  p_free(_physIdx);
  _physOwner = nullptr;
  _physIdx = nullptr;
  _physMapLen = 0;
  _physMapHash = 0;
  if (_length > 0) {
    _physOwner = static_cast<uint8_t*>(allocate_buffer(_length, BFRALLOC_PREFER_DRAM));
    _physIdx = static_cast<uint16_t*>(allocate_buffer((size_t)_length * sizeof(uint16_t), BFRALLOC_ENFORCE_PSRAM | BFRALLOC_CLEAR));
    if (_physOwner && _physIdx) {
      _physMapLen = _length;
      memset(_physOwner, PHYS_SEG_BLACK, _physMapLen);
    } else {
      p_free(_physOwner); _physOwner = nullptr;
      p_free(_physIdx);   _physIdx = nullptr;
    }
    DEBUG_PRINTF_P(PSTR("physMap buffer size: %uB\n"), (uint32_t)_length * 3);
  }
#endif
}

void WS2812FX::service() {
  unsigned long nowUp = millis(); // Be aware, millis() rolls over every 49 days
  unsigned long elapsed = nowUp - _lastServiceShow;
  bool timeToShow = (elapsed >= _frametime);                        // all segments are running at the same speed
  if (_triggered || _targetFps == FPS_UNLIMITED) timeToShow = true; // unlimited mode = no frametime; strip.trigger() can overrule timing

  now = nowUp + timebase;                               // common time base for all effects
  if (!timeToShow) return;                              // too early for service
  if (_suspend || elapsed <= MIN_FRAME_DELAY) return;   // keep wifi alive - no matter if triggered or unlimited

  _isServicing = true;
  bool doShow = _triggered;    // true if ≥1 active segment was processed (and strip was not suspended mid-loop), or trigger received → triggers show()
  for (size_t i = 0; i < _segments.size(); i++) {
    Segment &seg = _segments[i];
    _segment_index = i;
    if (_suspend) break; // abort processing segments if suspend requested during service()

    // process transition (also pre-calculates progress value)
    seg.handleTransition();
    // reset the segment runtime data if needed
    seg.resetIfRequired();

    if (seg.isActive()) {
      // current segment is active -> re-run effect, and remember that show() call is necessary
      // if we arrive here, its always showtime (timeToShow == true)
      doShow = true;
      if (!seg.freeze) { //only run effect function if not frozen
        // Effect blending
        uint16_t prog = seg.fadeProgress(); // color blending uses fade channel progress
        seg.beginDraw(prog);                // set up parameters for get/setPixelColor() (will also blend colors and palette)
        _currentSegment = &seg;             // set current segment for effect functions (SEGMENT & SEGENV)
        // workaround for on/off transition to respect blending style
        _mode[seg.mode]();                  // run new/current mode (needed for bri workaround)
        seg.call++;
        // if segment is in transition and no old segment exists we don't need to run the old mode
        // (blendSegments() takes care of On/Off transitions and clipping)
        Segment *segO = seg.getOldSegment();
        if (segO && segO->isActive() && (seg.mode != segO->mode || blendingStyle != TRANSITION_FADE ||
            (segO->name != seg.name && segO->name && seg.name && strncmp(segO->name, seg.name, WLED_MAX_SEGNAME_LEN) != 0))) {
          Segment::modeBlend(true);         // set flag for beginDraw() to blend colors and palette
          //segO->beginDraw(0xFFFFU);         // old segment renders its captured state (no fade), parent segment holds transition progress
          segO->beginDraw(prog);            // set up palette & colors (also sets draw dimensions), parent segment has transition progress
          _currentSegment = segO;           // set current segment
          // workaround for on/off transition to respect blending style
          _mode[segO->mode]();              // run old mode (needed for bri workaround; semaphore!!)
          segO->call++;                     // increment old mode run counter
          Segment::modeBlend(false);        // unset flag
        }
      }
    }
  }
  _segment_index = 0;     // segment index is only valid while effects are serviced
  _currentSegment = &_segments[0]; // safe fallback to prevent stale pointer - SEGMENT/SEGENV should not be used outside of the service loop

  #ifdef WLED_DEBUG
  if ((_targetFps != FPS_UNLIMITED) && (millis() - nowUp > _frametime)) DEBUG_PRINTF_P(PSTR("Slow effects %u/%d.\n"), (unsigned)(millis()-nowUp), (int)_frametime);
  #endif
  if (doShow && !_suspend) {
    yield();
    Segment::handleRandomPalette(); // slowly transition random palette; move it into for loop when each segment has individual random palette
    _lastServiceShow = nowUp; // update timestamp, for precise FPS control
    show();
  }
  #ifdef WLED_DEBUG
  if ((_targetFps != FPS_UNLIMITED) && (millis() - nowUp > _frametime)) DEBUG_PRINTF_P(PSTR("Slow strip %u/%d.\n"), (unsigned)(millis()-nowUp), (int)_frametime);
  #endif

  if (!_suspend) _triggered = false; // avoid losing "trigger" events if suspend requested during effect service()
  _isServicing = false;
}

// blend mode helpers and segblend() have moved to colors.h (shared with the bus layer)

static bool isStatePixelClipped1D(int i, uint16_t clipStart, uint16_t clipStop, uint8_t style, uint16_t spatialProg) {
  if (style != TRANSITION_FADE && clipStart != clipStop) {
    const bool invert = clipStart > clipStop;
    const int start = invert ? clipStop : clipStart;
    const int stop  = invert ? clipStart : clipStop;
    if (style == TRANSITION_FAIRY_DUST) {
      const unsigned len = stop - start;
      if (len < 2) return false;
      const unsigned shuffled = hashInt(i) % len;
      const unsigned pos = (shuffled * 0xFFFFU) / len;
      return (spatialProg <= pos);
    }
    const bool iInside = (i >= start && i < stop);
    return !iInside ^ invert;
  }
  return false;
}

static bool isStatePixelClippedXY(int x, int y, uint16_t clipStart, uint16_t clipStop, uint16_t clipStartY, uint16_t clipStopY, uint8_t style, uint16_t spatialProg) {
  if (style != TRANSITION_FADE && (clipStart != clipStop || clipStartY != clipStopY)) {
    const bool invertX = clipStart > clipStop;
    const bool invertY = clipStartY > clipStopY;
    const int cStartX = invertX ? clipStop   : clipStart;
    const int cStopX  = invertX ? clipStart  : clipStop;
    const int cStartY = invertY ? clipStopY  : clipStartY;
    const int cStopY  = invertY ? clipStartY : clipStopY;
    if (style == TRANSITION_FAIRY_DUST) {
      const unsigned width = cStopX - cStartX;
      const unsigned len = width * (cStopY - cStartY);
      if (len < 2) return false;
      const unsigned shuffled = hashInt(x + y * width) % len;
      const unsigned pos = (shuffled * 0xFFFFU) / len;
      return spatialProg <= pos;
    }
    if (style == TRANSITION_CIRCULAR_IN || style == TRANSITION_CIRCULAR_OUT) {
      const int cx = (cStopX - cStartX + 1) / 2;
      const int cy = (cStopY - cStartY + 1) / 2;
      const bool out = (style == TRANSITION_CIRCULAR_OUT);
      const unsigned prog = out ? spatialProg : 0xFFFFU - spatialProg;
      int radius2 = max(cx, cy) * prog / 0xFFFF;
      radius2 = 2 * radius2 * radius2;
      if (radius2 == 0) return out;
      const int dx = x - cx;
      const int dy = y - cy;
      const bool outside = dx * dx + dy * dy > radius2;
      return out ? outside : !outside;
    }
    bool xInside = (x >= cStartX && x < cStopX); if (invertX) xInside = !xInside;
    bool yInside = (y >= cStartY && y < cStopY); if (invertY) yInside = !yInside;
    const bool notclipped = (style == TRANSITION_OUTSIDE_IN) ? (xInside || yInside) : (xInside && yInside);
    return !notclipped;
  }
  return false;
}

uint16_t WS2812FX::panelPhysToLogical(uint16_t phys) const {
#ifndef WLED_DISABLE_2D
  if (!isMatrix) return phys;
  uint16_t pixStart = 0;
  for (const Panel &p : panel) {
    const uint16_t cnt = (uint16_t)p.width * p.height;
    if (phys < pixStart + cnt) {
      const uint16_t local = phys - pixStart;
      const uint16_t h = p.vertical ? p.height : p.width;
      const uint16_t v = p.vertical ? p.width  : p.height;
      const uint16_t j = local / h;
      const uint16_t i = local % h;
      uint16_t y = (p.vertical ? p.rightStart  : p.bottomStart) ? (v - j - 1) : j;
      uint16_t x = (p.vertical ? p.bottomStart : p.rightStart)  ? (h - i - 1) : i;
      if (p.serpentine && (j & 1)) x = h - x - 1;
      const uint16_t lx = p.xOffset + (p.vertical ? y : x);
      const uint16_t ly = p.yOffset + (p.vertical ? x : y);
      return ly * Segment::maxWidth + lx;
    }
    pixStart += cnt;
  }
  const unsigned matrixSize = (unsigned)Segment::maxWidth * Segment::maxHeight;
  if (phys >= matrixSize && phys < _length) return phys;
  return 0xFFFFU;
#else
  return phys;
#endif
}

uint16_t WS2812FX::panelLogicalToPhys(uint16_t logIdx) const {
#ifndef WLED_DISABLE_2D
  if (!isMatrix) return logIdx;
  const unsigned matrixSize = (unsigned)Segment::maxWidth * Segment::maxHeight;
  if (logIdx >= matrixSize) return (logIdx < _length) ? logIdx : 0xFFFFU;
  const uint16_t lx = logIdx % Segment::maxWidth;
  const uint16_t ly = logIdx / Segment::maxWidth;
  uint16_t pixStart = 0;
  for (const Panel &p : panel) {
    if (lx >= p.xOffset && lx < p.xOffset + p.width && ly >= p.yOffset && ly < p.yOffset + p.height) {
      const uint16_t px = lx - p.xOffset;
      const uint16_t py = ly - p.yOffset;
      const uint16_t h = p.vertical ? p.height : p.width;
      const uint16_t v = p.vertical ? p.width  : p.height;
      uint16_t x = p.vertical ? py : px;
      const uint16_t y = p.vertical ? px : py;
      const uint16_t j = (p.vertical ? p.rightStart : p.bottomStart) ? (v - y - 1) : y;
      if (p.serpentine && (j & 1)) x = h - x - 1;
      const uint16_t i = (p.vertical ? p.bottomStart : p.rightStart) ? (h - x - 1) : x;
      return pixStart + j * h + i;
    }
    pixStart += (uint16_t)p.width * p.height;
  }
  return 0xFFFFU;
#else
  return logIdx;
#endif
}

uint16_t WS2812FX::findLogicalPixel(uint16_t physIdx) const {
  if (physIdx >= _length) return 0xFFFFU;
  if (_invMappingTable && physIdx < _invMappingSize) return _invMappingTable[physIdx];
  if (isMatrix && !_hasCustomFileMap && !_hasPanelGaps) return panelPhysToLogical(physIdx);
  if (!_hasCustomFileMap && !isMatrix) return physIdx;
  if (customMappingTable) {
    for (uint16_t l = 0; l < customMappingSize; l++) {
      if (customMappingTable[l] == physIdx) return l;
    }
    return 0xFFFFU;
  }
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
  // Fallback when _invMappingTable was released after _physMap build: try reloading transient map or matching segment
  if (_physOwner && _physIdx && physIdx < _physMapLen) {
    const uint8_t owner = _physOwner[physIdx];
    if (owner == PHYS_SEG_BLACK) return 0xFFFFU;
    if (owner > PHYS_SEG_BLACK) {
      const uint16_t v = _physIdx[physIdx];
      return (owner > PHYS_SEG_COMPOSE) ? (uint16_t)((uint16_t)(v >> 8) * Segment::maxWidth + (uint16_t)(v & 0xFFu)) : v;
    }
  }
#endif
  return physIdx;
}

uint16_t WS2812FX::findPhysicalPixel(uint16_t logIdx) const {
  if (customMappingTable) {
    return (logIdx < customMappingSize) ? customMappingTable[logIdx] : 0xFFFFU;
  }
  if (!isMatrix && !_hasCustomFileMap) return (logIdx < _length) ? logIdx : 0xFFFFU;
  if (isMatrix && !_hasCustomFileMap && !_hasPanelGaps) return panelLogicalToPhys(logIdx);
  if (_invMappingTable) {
    for (uint16_t p = 0; p < _invMappingSize; p++) {
      if (_invMappingTable[p] == logIdx) return p;
    }
    return 0xFFFFU;
  }
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
  if (_physOwner && _physIdx) {
    // Check if logIdx matches a PHYS_SEG_COMPOSE entry directly
    for (uint16_t p = 0; p < _physMapLen; p++) {
      const uint8_t owner = _physOwner[p];
      if (owner > PHYS_SEG_BLACK) {
        const uint16_t v = _physIdx[p];
        const uint16_t l = (owner > PHYS_SEG_COMPOSE) ? (uint16_t)((uint16_t)(v >> 8) * Segment::maxWidth + (uint16_t)(v & 0xFFu)) : v;
        if (l == logIdx) return p;
      }
    }
    // Otherwise match against covering top segment's virtual index
    const uint32_t matrixSize = (uint32_t)Segment::maxWidth * Segment::maxHeight;
    const bool is2DCoord = isMatrix && (logIdx < matrixSize);
    const uint16_t x = is2DCoord ? (logIdx % Segment::maxWidth) : 0;
    const uint16_t y = is2DCoord ? (logIdx / Segment::maxWidth) : 0;
    uint8_t activeIdx = 0;
    int matchSeg = -1;
    uint16_t matchPix = 0;
    for (const Segment &seg : _segments) {
      if (!seg.isActive() || (!seg.on && !seg.isInTransition())) continue;
      const uint16_t len = seg.length();
      const bool seg2D = isMatrix && ((size_t)seg.start + (size_t)seg.startY * Segment::maxWidth + len <= matrixSize);
      const uint16_t nCols = seg2D ? seg.virtualWidth() : seg.virtualLength();
      const uint8_t  nRows = seg2D ? seg.virtualHeight() : 1;
      const uint8_t  gl = seg.groupLength();
      const uint16_t off = len ? (seg.offset % len) : 0;
      uint16_t c = 0, r = 0;
      if (resolveRawVirtualCoord(seg2D, seg.start, seg.stop, (uint8_t)seg.startY, (uint8_t)seg.stopY,
                                 seg.width(), (uint8_t)seg.height(), len, off, nCols, nRows,
                                 seg.grouping, gl, (gl == 1) && !seg.mirror && !seg.mirror_y,
                                 seg.transpose, seg.mirror, seg.mirror_y, seg.reverse, seg.reverse_y,
                                 logIdx, x, y, is2DCoord, c, r)) {
        matchSeg = activeIdx;
        matchPix = c + r * nCols;
      }
      activeIdx++;
    }
    if (matchSeg >= 0) {
      for (uint16_t p = 0; p < _physMapLen; p++) {
        if (_physOwner[p] == matchSeg && _physIdx[p] == matchPix) return p;
      }
    }
  }
#endif
  return 0xFFFFU;
}

bool WS2812FX::resolveRawVirtualCoord(bool is2DSeg, uint16_t start, uint16_t stop, uint8_t startY, uint8_t stopY,
                                      uint16_t width, uint8_t height, uint16_t length, uint16_t offset,
                                      uint16_t nCols, uint8_t nRows, uint8_t grouping, uint8_t groupLen,
                                      bool isSimple, bool transpose, bool mirror, bool mirror_y, bool reverse, bool reverse_y,
                                      uint16_t logIdx, uint16_t x, uint16_t y, bool is2DCoord, uint16_t &outC, uint16_t &outR) {
  if (is2DSeg) {
    if (!is2DCoord || x < start || x >= stop || y < startY || y >= stopY) return false;
    uint16_t mx = x - start;
    uint16_t my = y - startY;
    if (!isSimple) {
      const bool flipX = transpose ? mirror_y : mirror;
      const bool flipY = transpose ? mirror   : mirror_y;
      if (flipX && mx >= ((width  + 1) >> 1)) mx = width  - mx - 1;
      if (flipY && my >= ((height + 1) >> 1)) my = height - my - 1;
      if (groupLen > 1) {
        if ((mx % groupLen) >= grouping || (my % groupLen) >= grouping) return false;
        mx /= groupLen;
        my /= groupLen;
      }
    }
    if (transpose) std::swap(mx, my);
    if (mx >= nCols || my >= nRows) return false;
    if (reverse)   mx = nCols - mx - 1;
    if (reverse_y) my = nRows - my - 1;
    outC = mx;
    outR = my;
    return true;
  } else {
    if (logIdx < start || logIdx >= stop) return false;
    uint16_t rel = logIdx - start;
    if (offset) rel = (rel >= offset) ? (rel - offset) : (rel + length - offset);
    if (!isSimple) {
      if (mirror && rel >= ((length + 1) >> 1)) rel = length - rel - 1;
      if (groupLen > 1) {
        if ((rel % groupLen) >= grouping) return false;
        rel /= groupLen;
      }
    }
    if (rel >= nCols) return false;
    if (reverse) rel = nCols - rel - 1;
    outC = rel;
    outR = 0;
    return true;
  }
}

inline bool WS2812FX::resolveSegmentVirtualCoord(const SegRenderState &s, uint16_t logIdx, uint16_t x, uint16_t y, bool is2DCoord, uint16_t &outC, uint16_t &outR) {
  return resolveRawVirtualCoord(s.is2DSeg, s.start, s.stop, s.startY, s.stopY, s.width(), s.height(), s.length(), s.offset, s.nCols, s.nRows, s.grouping, s.groupLen, s.isSimple, s.transpose, s.mirror, s.mirror_y, s.reverse, s.reverse_y, logIdx, x, y, is2DCoord, outC, outR);
}

uint8_t WS2812FX::countActiveSegments() const {
  uint8_t n = 0;
  for (const Segment &seg : _segments) {
    if (!seg.isActive() || (!seg.on && !seg.isInTransition())) continue;
    if (n >= MAX_NUM_SEGMENTS) break;
    n++;
  }
  return n;
}

bool WS2812FX::ensureRenderBuffers(uint8_t numSegments) const {
  if (numSegments < 1) numSegments = 1;
  if (_stateBuf && _planBuf && _renderBufCap >= numSegments) return true;
  _stateCacheValid = false;
  _cachedNumActive = 0;
  d_free(_stateBuf);
  d_free(_planBuf);
  _stateBuf = nullptr;
  _planBuf = nullptr;
  _renderBufCap = 0;
  _stateBuf = static_cast<SegRenderState*>(d_malloc((size_t)numSegments * sizeof(SegRenderState)));
  _planBuf = static_cast<SegPlan*>(d_malloc((size_t)numSegments * sizeof(SegPlan)));
  if (_stateBuf && _planBuf) {
    _renderBufCap = numSegments;
    return true;
  }
  d_free(_stateBuf); _stateBuf = nullptr;
  d_free(_planBuf);  _planBuf = nullptr;
  DEBUG_PRINTF_P(PSTR("render scratch buffers: allocation of %u segments failed\n"), (unsigned)numSegments);
  return false;
}

bool WS2812FX::ensureCachedStates() const {
  if (_stateCacheValid && _stateBuf) return true;
  if (!ensureRenderBuffers(countActiveSegments())) return false;
  bool dummyUniform = true;
  uint8_t dummyCCT = 127;
  buildFrameStates(_stateBuf, _renderBufCap, nullptr, nullptr, nullptr, _cachedNumActive, dummyUniform, dummyCCT);
  _stateCacheValid = true;
  return true;
}

void WS2812FX::buildFrameStates(SegRenderState* states, uint8_t capacity, const uint32_t** segPixels, uint8_t* segOpacity, uint8_t* segCCT, uint8_t &numActive, bool &uniformCCT, uint8_t &uniformCCTVal) const {
  numActive = 0;
  uniformCCT = true;
  uniformCCTVal = 127;
  int16_t firstCCT = -1;
  const bool checkCCT = (hasCCTBus() || correctWB) && !cctFromRgb;
  const size_t matrixSize = (size_t)Segment::maxWidth * Segment::maxHeight;

  for (const Segment &seg : _segments) {
    if (!seg.isActive() || (!seg.on && !seg.isInTransition())) continue;
    if (numActive >= capacity) break;

    SegRenderState &s = states[numActive];
    s.pixels = seg.pixels;
    s.segO = seg.getOldSegment();
    s.pixelsO = s.segO ? s.segO->pixels : nullptr;
    s.start = seg.start;
    s.stop = seg.stop;
    s.startY = (uint8_t)seg.startY;
    s.stopY = (uint8_t)seg.stopY;
    const uint16_t w = s.width();
    const uint8_t  h = s.height();
    const uint16_t len = s.length();
    s.is2DSeg = isMatrix && ((size_t)s.start + (size_t)s.startY * Segment::maxWidth + len <= matrixSize);
    s.grouping = seg.grouping;
    s.groupLen = seg.groupLength();
    s.reverse = seg.reverse;
    s.reverse_y = seg.reverse_y;
    s.transpose = seg.transpose;
    s.mirror = seg.mirror;
    s.mirror_y = seg.mirror_y;
    s.offset = len ? (seg.offset % len) : 0;
    s.blendMode = seg.blendMode < BLENDMODES ? seg.blendMode : 0;
    s.map1D2D   = seg.map1D2D;
    s.cct = seg.currentCCT();

    uint8_t opacity = gamma8inv(seg.currentBri());
    uint8_t opacityOld = opacity;
    uint8_t style = blendingStyle;
    if (s.segO && style != TRANSITION_FADE) opacityOld = gamma8inv(s.segO->currentBri());
    else opacityOld = opacity;
    if ((w * h) == 1 || !s.segO) style = TRANSITION_FADE;
    s.opacity = opacity;
    s.opacityOld = opacityOld;
    s.style = style;

    if (s.is2DSeg) {
      s.nCols = seg.virtualWidth();
      s.nRows = seg.virtualHeight();
      s.oCols = s.segO ? s.segO->virtualWidth() : s.nCols;
      s.oRows = s.segO ? s.segO->virtualHeight() : s.nRows;
    } else {
      s.nCols = seg.virtualLength();
      s.nRows = 1;
      s.oCols = s.segO ? s.segO->virtualLength() : s.nCols;
      s.oRows = 1;
    }

    s.spatialProg = seg.spatialProgress();
    const unsigned progress = (style == TRANSITION_OUTSIDE_IN) ? Segment::invertProgress(s.spatialProg) : s.spatialProg;
    s.progInv = Segment::invertProgress(progress);
    s.invertClipping = (style != TRANSITION_FADE) && seg.isTransitionReversed();
    s.powerOffTrans = seg.isPowerOffTransition();
    s.powerOnTrans = seg.isPowerOnTransition();
    s.fadeOldDiffMode = (s.segO && style == TRANSITION_FADE &&
      (seg.mode != s.segO->mode || (s.is2DSeg && s.segO->name != seg.name && s.segO->name && seg.name && strncmp(s.segO->name, seg.name, WLED_MAX_SEGNAME_LEN) != 0)));

    s.clipStart = s.clipStop = s.clipStartY = s.clipStopY = 0;
    s.pushOffsetX = s.pushOffsetY = 0;
    if (s.segO && style != TRANSITION_FADE) {
      const unsigned dw = (progress * w) / 0xFFFFU + 1;
      const unsigned dh = (progress * h) / 0xFFFFU + 1;
      switch (style) {
        case TRANSITION_CIRCULAR_IN:
        case TRANSITION_CIRCULAR_OUT:
        case TRANSITION_FAIRY_DUST:
          s.clipStop = w; s.clipStopY = h; break;
        case TRANSITION_SWIPE_RIGHT:
        case TRANSITION_PUSH_RIGHT:
          s.clipStop = dw; s.clipStopY = h; break;
        case TRANSITION_SWIPE_LEFT:
        case TRANSITION_PUSH_LEFT:
          s.clipStart = w - dw; s.clipStop = w; s.clipStopY = h; break;
        case TRANSITION_OUTSIDE_IN:
          s.clipStart = (w + dw) / 2; s.clipStop = (w - dw) / 2;
          s.clipStartY = (h + dh) / 2; s.clipStopY = (h - dh) / 2; break;
        case TRANSITION_INSIDE_OUT:
          s.clipStart = (w - dw) / 2; s.clipStop = (w + dw) / 2;
          s.clipStartY = (h - dh) / 2; s.clipStopY = (h + dh) / 2; break;
        case TRANSITION_SWIPE_DOWN:
        case TRANSITION_PUSH_DOWN:
          s.clipStop = w; s.clipStopY = dh; break;
        case TRANSITION_SWIPE_UP:
        case TRANSITION_PUSH_UP:
          s.clipStop = w; s.clipStartY = h - dh; s.clipStopY = h; break;
        case TRANSITION_OPEN_H:
          s.clipStart = (w - dw) / 2; s.clipStop = (w + dw) / 2; s.clipStopY = h; break;
        case TRANSITION_OPEN_V:
          s.clipStop = w; s.clipStartY = (h - dh) / 2; s.clipStopY = (h + dh) / 2; break;
        case TRANSITION_SWIPE_TL:
        case TRANSITION_PUSH_TL:
          s.clipStop = dw; s.clipStopY = dh; break;
        case TRANSITION_SWIPE_TR:
        case TRANSITION_PUSH_TR:
          s.clipStart = w - dw; s.clipStop = w; s.clipStopY = dh; break;
        case TRANSITION_SWIPE_BR:
        case TRANSITION_PUSH_BR:
          s.clipStart = w - dw; s.clipStop = w; s.clipStartY = h - dh; s.clipStopY = h; break;
        case TRANSITION_SWIPE_BL:
        case TRANSITION_PUSH_BL:
          s.clipStop = dw; s.clipStartY = h - dh; s.clipStopY = h; break;
      }
      if (s.is2DSeg) {
        const unsigned offX = (style == TRANSITION_PUSH_UP || style == TRANSITION_PUSH_DOWN) ? 0 : (unsigned)s.progInv * s.nCols / 0xFFFFU;
        const unsigned offY = (style == TRANSITION_PUSH_LEFT || style == TRANSITION_PUSH_RIGHT) ? 0 : (unsigned)s.progInv * s.nRows / 0xFFFFU;
        switch (style) {
          case TRANSITION_PUSH_RIGHT: s.pushOffsetX = offX; break;
          case TRANSITION_PUSH_LEFT:  s.pushOffsetX = (s.nCols - offX) % s.nCols; break;
          case TRANSITION_PUSH_DOWN:  s.pushOffsetY = offY; break;
          case TRANSITION_PUSH_UP:    s.pushOffsetY = (s.nRows - offY) % s.nRows; break;
          case TRANSITION_PUSH_TL:    s.pushOffsetX = offX; s.pushOffsetY = offY; break;
          case TRANSITION_PUSH_TR:    s.pushOffsetX = (s.nCols - offX) % s.nCols; s.pushOffsetY = offY; break;
          case TRANSITION_PUSH_BR:    s.pushOffsetX = (s.nCols - offX) % s.nCols; s.pushOffsetY = (s.nRows - offY) % s.nRows; break;
          case TRANSITION_PUSH_BL:    s.pushOffsetX = offX; s.pushOffsetY = (s.nRows - offY) % s.nRows; break;
        }
      } else {
        const unsigned offI = (unsigned)s.progInv * s.nCols / 0xFFFFU;
        if (style == TRANSITION_PUSH_RIGHT) s.pushOffsetX = offI % s.nCols;
        else if (style == TRANSITION_PUSH_LEFT) s.pushOffsetX = (s.nCols - (offI % s.nCols)) % s.nCols;
      }
    }

    s.isSimple = !s.segO && (s.groupLen == 1) && !s.mirror && !s.mirror_y;

    if (checkCCT) {
      if (firstCCT < 0) {
        firstCCT = s.cct;
        uniformCCTVal = s.cct;
      } else if ((uint8_t)firstCCT != s.cct) {
        uniformCCT = false;
      }
    }

    if (segPixels)  segPixels[numActive]  = s.pixels;
    if (segOpacity) segOpacity[numActive] = s.opacity;
    if (segCCT)     segCCT[numActive]     = s.cct;
    numActive++;
  }
}

// Builds the affine render plan used by the bus layer's fast-path compose.
// The plan describes, for every active segment, how a logical pixel index maps onto an
// index inside the segment's own pixel buffer. For "simple" segments this mapping is
// affine (idx = base + tx*x + ty*y), so all per-pixel coordinate arithmetic can be
// hoisted out of the pixel loop.
// Returns false if any active segment cannot be expressed as an affine map - in that
// case the generic compose (WS2812FX::composeLogicalPixel) is used for composed pixels.
bool WS2812FX::buildSegPlans(const SegRenderState* states, uint8_t numActive, SegPlan* plans, uint8_t &numPlans) const {
  numPlans = 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < numActive; i++) {
    const SegRenderState &s = states[i];
    if (!s.pixels) return false;                // missing pixel buffer
    if (s.segO) return false;                   // transition: two sources per segment
    if (s.groupLen != 1) return false;         // grouping/spacing replicates pixels
    if (s.mirror || s.mirror_y) return false;   // mirrored segments are only piecewise affine

    SegPlan &p = plans[n];
    p.pixels   = s.pixels;
    p.opacity  = s.opacity;
    p.blendMode = s.blendMode;
    p.cct      = s.cct;

    if (s.is2DSeg) {
      if (s.transpose) return false;                       // transposed mapping swaps X & Y
      if (s.map1D2D != M12_Pixels) return false;           // pBar/pArc/pCorner/pSpinwheel are not affine
      if (s.nCols != s.width() || s.nRows != s.height()) return false; // grouping/spacing
      const int32_t sx = s.reverse   ? -1 : 1;
      const int32_t sy = s.reverse_y ? -1 : 1;
      p.tx   = sx;
      p.ty   = (int32_t)s.nCols * sy;
      p.base = (s.reverse ? (int32_t)s.nCols - 1 : 0)
             + (int32_t)s.nCols * (s.reverse_y ? (int32_t)s.nRows - 1 : 0)
             - sx * (int32_t)s.start
             - sy * (int32_t)s.startY * (int32_t)s.nCols;
      p.x0 = s.start;  p.x1 = s.stop;
      p.y0 = s.startY; p.y1 = s.stopY;
    } else {
      if (s.offset) return false;                // rotated segment: not affine
      if (s.nCols != s.length()) return false;   // grouping/spacing
      p.tx   = s.reverse ? -1 : 1;
      p.ty   = 0;
      p.base = s.reverse ? ((int32_t)s.nCols - 1 + (int32_t)s.start) : -(int32_t)s.start;
      p.x0 = s.start;  p.x1 = s.stop;
      p.y0 = 0;        p.y1 = 1;
    }
    n++;
  }
  numPlans = n;
  return true;
}

uint32_t WS2812FX::composeLogicalPixel(uint32_t logOrXY, bool is2DXY, const SegRenderState* states, uint8_t numActive, uint8_t &outCCT) const {
  if (numActive == 0) return BLACK;

  const uint16_t matrixW = Segment::maxWidth;
  const uint32_t matrixSize = (uint32_t)matrixW * Segment::maxHeight;
  uint16_t x = 0, y = 0, logIdx = 0;
  bool is2DCoord = false;
  if (is2DXY) {
    x = logOrXY & 0xFFu;
    y = (logOrXY >> 8) & 0xFFu;
    logIdx = y * matrixW + x;
    is2DCoord = true;
  } else {
    logIdx = (uint16_t)logOrXY;
    is2DCoord = isMatrix && (logIdx < matrixSize);
    if (is2DCoord) {
      x = logIdx % matrixW;
      y = logIdx / matrixW;
    }
  }

  uint32_t accum = BLACK;
  for (uint8_t idx = 0; idx < numActive; idx++) {
    const SegRenderState &s = states[idx];
    uint16_t c = 0, r = 0;
    if (!resolveSegmentVirtualCoord(s, logIdx, x, y, is2DCoord, c, r)) continue;

    bool clipped = false;
    uint8_t pixelOpacity = s.opacity;
    if (s.segO && s.style != TRANSITION_FADE) {
      if (s.is2DSeg) {
        clipped = isStatePixelClippedXY(c, r, s.clipStart, s.clipStop, s.clipStartY, s.clipStopY, s.style, s.spatialProg) ^ s.invertClipping;
      } else {
        clipped = isStatePixelClipped1D(c, s.clipStart, s.clipStop, s.style, s.spatialProg) ^ s.invertClipping;
      }
      if (clipped) pixelOpacity = s.opacityOld;
    }

    const bool useOld = clipped && s.segO;
    uint16_t sx = c, sy = r;
    if (s.pushOffsetX) sx = (sx + s.pushOffsetX) % s.nCols;
    if (s.pushOffsetY) sy = (sy + s.pushOffsetY) % s.nRows;

    uint32_t c_a = BLACK;
    if (!useOld) {
      if (s.pixels && sx < s.nCols && sy < s.nRows) c_a = s.pixels[sx + sy * s.nCols];
    } else {
      if (s.pixelsO && sx < s.oCols && sy < s.oRows) c_a = s.pixelsO[sx + sy * s.oCols];
    }

    if (s.fadeOldDiffMode) {
      if (s.pixelsO && sx < s.oCols && sy < s.oRows) {
        c_a = color_blend16(c_a, s.pixelsO[sx + sy * s.oCols], s.progInv);
      }
    } else if (s.style != TRANSITION_FADE) {
      if ((s.powerOffTrans && !clipped) || (s.powerOnTrans && clipped)) c_a = BLACK;
    }

    outCCT = s.cct;
    if (pixelOpacity == 0) continue;
    if (s.blendMode == 0 && pixelOpacity == 255) {
      accum = c_a;
    } else {
      const uint32_t blended = segblend(s.blendMode, c_a, accum);
      if (pixelOpacity == 255) accum = blended;
      else accum = accum ? color_blend(accum, blended, pixelOpacity) : fast_color_scale(blended, (uint8_t)(pixelOpacity + 1));
    }
  }
  return accum;
}

uint32_t WS2812FX::composeSlowCallback(const void* fxPtr, uint32_t logOrXY, bool is2DXY, uint8_t &outCCT) {
  const WS2812FX* fx = static_cast<const WS2812FX*>(fxPtr);
  return fx->composeLogicalPixel(logOrXY, is2DXY, fx->_frameStates, fx->_frameNumActive, outCCT);
}

#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
void WS2812FX::rebuildPhysMap() {
  if (!_physOwner || !_physIdx || _physMapLen == 0) return;

  if (!ensureRenderBuffers(countActiveSegments())) return;
  SegRenderState* const states = _stateBuf;
  uint8_t numActive = 0;
  bool dummyUniform = true;
  uint8_t dummyCCT = 127;
  buildFrameStates(states, _renderBufCap, nullptr, nullptr, nullptr, numActive, dummyUniform, dummyCCT);
  _cachedNumActive = numActive;
  _stateCacheValid = true;

  bool loadedTransientInv = false;
  if (!_invMappingTable && (_hasCustomFileMap || _hasPanelGaps)) {
    if (_hasCustomFileMap) loadedTransientInv = loadInvMapFromFile(currentLedmap);
    else if (isMatrix) {
      setUpMatrix(); // populates _invMappingTable for /2d-gaps.json
      loadedTransientInv = (_invMappingTable != nullptr);
    }
  }

  const uint16_t matrixW = Segment::maxWidth;
  const uint32_t matrixSize = (uint32_t)matrixW * Segment::maxHeight;
  const bool usePanelMath = isMatrix && !_invMappingTable && !_hasCustomFileMap && !_hasPanelGaps;

  for (uint16_t phys = 0; phys < _physMapLen; phys++) {
    uint16_t logIdx = phys;
    if (_invMappingTable) {
      logIdx = (phys < _invMappingSize) ? _invMappingTable[phys] : 0xFFFFU;
    } else if (usePanelMath) {
      logIdx = panelPhysToLogical(phys);
    }
    if (logIdx == 0xFFFFU) {
      _physOwner[phys] = PHYS_SEG_BLACK;
      _physIdx[phys] = 0;
      continue;
    }

    const bool is2DCoord = isMatrix && (logIdx < matrixSize);
    const uint16_t x = is2DCoord ? (logIdx % matrixW) : 0;
    const uint16_t y = is2DCoord ? (logIdx / matrixW) : 0;

    uint8_t ownerSeg = PHYS_SEG_BLACK;
    uint16_t ownerPix = 0;
    bool needsCompose = false;

    for (uint8_t sIdx = 0; sIdx < numActive; sIdx++) {
      const SegRenderState &s = states[sIdx];
      uint16_t c = 0, r = 0;
      if (!resolveSegmentVirtualCoord(s, logIdx, x, y, is2DCoord, c, r)) continue;

      if (s.blendMode == 0 && !s.segO && (s.opacity == 255 || ownerSeg == PHYS_SEG_BLACK)) {
        needsCompose = false;
        ownerSeg = sIdx;
        ownerPix = c + r * s.nCols;
      } else {
        needsCompose = true;
        ownerSeg = PHYS_SEG_COMPOSE;
      }
    }

    if (needsCompose) {
      // PHYS_SEG_COMPOSE + 0 = 1D, PHYS_SEG_COMPOSE + 1 = 2D
      _physOwner[phys] = (uint8_t)(PHYS_SEG_COMPOSE + (is2DCoord ? 1u : 0u));
      if (is2DCoord) {
        _physIdx[phys] = (uint16_t)((uint16_t)x | (uint16_t)(y << 8));
      } else {
        _physIdx[phys] = logIdx;
      }
    } else if (ownerSeg == PHYS_SEG_BLACK) {
      _physOwner[phys] = PHYS_SEG_BLACK;
      _physIdx[phys] = 0;
    } else {
      _physOwner[phys] = ownerSeg;
      _physIdx[phys] = ownerPix;
    }
  }

  // Free transient _invMappingTable in _physMap builds so max RAM stays <= 3B/px
  if (_invMappingTable) {
    d_free(_invMappingTable);
    _invMappingTable = nullptr;
    _invMappingSize = 0;
    if (loadedTransientInv) {} // silence unused warning if WLED_DEBUG disabled
  }
}

void WS2812FX::ensurePhysMapValid() {
  if (!_physOwner || !_physIdx || _physMapLen != _length) updatePixelBuffer();
  if (!_physOwner || !_physIdx) return;

  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
  mix(_length);
  mix(_logicalLength);
  mix(Segment::maxWidth);
  mix(Segment::maxHeight);
  mix((uint32_t)isMatrix | ((uint32_t)_hasCustomFileMap << 1) | ((uint32_t)_hasPanelGaps << 2) | ((uint32_t)currentLedmap << 8));
  uint8_t sIdx = 0;
  for (const Segment &seg : _segments) {
    if (!seg.isActive() || (!seg.on && !seg.isInTransition())) continue;
    mix(sIdx++);
    mix(((uint32_t)seg.start << 16) | seg.stop);
    mix(((uint32_t)seg.startY << 16) | seg.stopY);
    mix(((uint32_t)seg.offset << 16) | ((uint32_t)seg.grouping << 8) | seg.spacing);
    const uint32_t flags = (uint32_t)seg.reverse | ((uint32_t)seg.reverse_y << 1) | ((uint32_t)seg.transpose << 2) |
                           ((uint32_t)seg.mirror << 3) | ((uint32_t)seg.mirror_y << 4) |
                           ((uint32_t)(seg.blendMode == 0) << 5) | ((uint32_t)(seg.currentBri() == 255) << 6) |
                           ((uint32_t)seg.isInTransition() << 7) |
                           ((uint32_t)(seg.map1D2D & 0x07) << 8); // affects the affine render plan
    mix(flags);
  }
  if (h == 0) h = 1;
  if (h != _physMapHash) {
    _physMapHash = h;
    rebuildPhysMap();
  }
}
#endif

void WS2812FX::prepareRealtimeMapping(bool enterRealtime) {
  if (enterRealtime) {
    if (customMappingTable || (!_hasCustomFileMap && !_hasPanelGaps)) return;
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    p_free(_physOwner);
    p_free(_physIdx);
    _physOwner = nullptr;
    _physIdx = nullptr;
    _physMapLen = 0;
    _physMapHash = 0;
#endif
    d_free(_invMappingTable);
    _invMappingTable = nullptr;
    _invMappingSize = 0;
    loadForwardMapFromFile(currentLedmap);
  } else {
    if (!customMappingTable && _invMappingTable) return;
    if (customMappingTable) {
      d_free(customMappingTable);
      customMappingTable = nullptr;
      customMappingSize = 0;
    }
#ifdef WLED_DISABLE_GLOBAL_PIXELBUFFER
    if (_hasCustomFileMap) loadInvMapFromFile(currentLedmap);
    else if (isMatrix) setUpMatrix();
#else
    updatePixelBuffer();
    ensurePhysMapValid();
#endif
  }
}

void WS2812FX::show() {
  BusManager::updateGammaUse(); // update gamma use in case something changed
  unsigned long showNow = millis();
  size_t diff = showNow - _lastShow;

  const bool renderSegments = (realtimeMode == REALTIME_MODE_INACTIVE || useMainSegmentOnly || realtimeOverride > REALTIME_OVERRIDE_NONE);
  if (renderSegments) {
    if (customMappingTable != nullptr) prepareRealtimeMapping(false);
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    ensurePhysMapValid();
#endif
  }

  while (!BusManager::canAllShow()) yield(); // wait for all buses to finish sending the previous frame

  // Keep the previous frame if render scratch allocation fails.
  if (renderSegments && ensureRenderBuffers(countActiveSegments())) {
    SegRenderState* const states = _stateBuf;
    SegPlan* const plans = _planBuf;
    uint8_t numPlans = 0;
    uint8_t numActive = 0;
    bool uniformCCT = true;
    uint8_t uniformCCTVal = 127;

#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    const uint32_t* segPixels[MAX_NUM_SEGMENTS];
    uint8_t segOpacity[MAX_NUM_SEGMENTS];
    uint8_t segCCT[MAX_NUM_SEGMENTS];
    buildFrameStates(states, _renderBufCap, segPixels, segOpacity, segCCT, numActive, uniformCCT, uniformCCTVal);
#else
    buildFrameStates(states, _renderBufCap, nullptr, nullptr, nullptr, numActive, uniformCCT, uniformCCTVal);
#endif
    _cachedNumActive = numActive;
    _stateCacheValid = true;
    // hoisted, frame-invariant render plan: lets the bus layer compose pixels without
    // re-deriving segment coordinates for every pixel
    const bool allAffine = buildSegPlans(states, numActive, plans, numPlans);

    _frameStates    = states;
    _frameNumActive = numActive;

    PullPixelContext ctx{};
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    ctx.segPixels   = segPixels;
    ctx.segOpacity  = segOpacity;
    ctx.segCCT      = segCCT;
    ctx.physOwner   = _physOwner;
    ctx.physIdx     = _physIdx;
    ctx.physMapLen  = (_physOwner && _physIdx) ? _physMapLen : 0;
#else
    ctx.invMap      = _invMappingTable;
    ctx.invMapLen   = _invMappingSize;
    ctx.matrixW     = Segment::maxWidth;
    ctx.matrixSize  = isMatrix ? ((uint32_t)Segment::maxWidth * Segment::maxHeight) : 0;
#endif
    ctx.numActive   = numActive;
    ctx.uniformCCT  = uniformCCT || cctFromRgb;
    ctx.correctWB   = correctWB;
    ctx.cctFromRgb  = cctFromRgb;
    ctx.composeSlow = WS2812FX::composeSlowCallback;
    ctx.fxPtr       = this;
    ctx.plans       = plans;
    ctx.numPlans    = numPlans;
    ctx.useAffine   = allAffine;
    ctx.linear1D    = !isMatrix && !_hasCustomFileMap && !_hasPanelGaps;

    const int oldCCT = Bus::getCCT();
    if (cctFromRgb) BusManager::setSegmentCCT(-1);
    else if (uniformCCT) BusManager::setSegmentCCT(uniformCCTVal, correctWB);

    BusManager::updateFromBuffer(ctx);
    Bus::setCCT(oldCCT);

    _frameStates    = nullptr;
    _frameNumActive = 0;
  }

  // Paint overlays directly into bus encode buffers (after segment pull, before ABL & show)
  handleOverlayDraw();

  // Pass the pixels on to the buses and start sending them out
  BusManager::show();

  if (diff > 0) { // skip calculation if no time has passed
    size_t fpsCurr = (1000 << FPS_CALC_SHIFT) / diff;
    _cumulativeFps = (FPS_CALC_AVG * _cumulativeFps + fpsCurr + FPS_CALC_AVG / 2) / (FPS_CALC_AVG + 1);
    _lastShow = showNow;
  }
}

void WS2812FX::setPixelColor(unsigned n, uint32_t c) const {
  const uint16_t phys = (realtimeMode == REALTIME_MODE_INACTIVE || realtimeRespectLedMaps) ? findPhysicalPixel(n) : (uint16_t)n;
  if (phys < _length) BusManager::setPixelColor(phys, c);
}

uint32_t WS2812FX::getPixelColor(unsigned n) const {
  const uint16_t total = getLengthTotal();
  if (n >= total) return 0;
  if (realtimeMode == REALTIME_MODE_INACTIVE || useMainSegmentOnly || realtimeOverride > REALTIME_OVERRIDE_NONE) {
    if (!ensureCachedStates()) return 0;
    uint8_t dummyOutCCT = 127;
    return composeLogicalPixel(n, false, _stateBuf, _cachedNumActive, dummyOutCCT);
  }
  const uint16_t phys = realtimeRespectLedMaps ? findPhysicalPixel(n) : (uint16_t)n;
  return (phys < _length) ? BusManager::getPixelColor(phys) : 0;
}

uint32_t WS2812FX::getPixelColorNoMap(unsigned n) const {
  if (n >= _length) return 0;
  if (realtimeMode == REALTIME_MODE_INACTIVE || useMainSegmentOnly || realtimeOverride > REALTIME_OVERRIDE_NONE) {
    const uint16_t logIdx = findLogicalPixel(n);
    if (logIdx >= getLengthTotal()) return 0;
    if (!ensureCachedStates()) return 0;
    uint8_t dummyOutCCT = 127;
    return composeLogicalPixel(logIdx, false, _stateBuf, _cachedNumActive, dummyOutCCT);
  }
  return BusManager::getPixelColor(n);
}

void WS2812FX::setRealtimePixelColor(unsigned i, uint32_t c) {
  if (useMainSegmentOnly) {
    const Segment &seg = getMainSegment();
    if (seg.isActive() && i < seg.length()) seg.setPixelColorRaw(i, c);
  } else {
    if (!realtimeOverride && realtimeRespectLedMaps && !customMappingTable && (_hasCustomFileMap || _hasPanelGaps)) {
      prepareRealtimeMapping(true);
    }
    setPixelColor(i, c);
  }
}

// reset all segments
void WS2812FX::restartRuntime() {
  suspend();
  waitForIt();
  for (Segment &seg : _segments) seg.markForReset().resetIfRequired();
  resume();
}

// start global power on/off or stop transition for all segments
void WS2812FX::setTransitionMode(bool start) {
  suspend();
  waitForIt();
  _stateCacheValid = false;
  for (Segment &seg : _segments) {
    if (start) seg.startTransition(_transitionDur, TRANSITION_KIND_DEFAULT | _poweringOnOff); // set color kind to let startTransition() determine if we need a segment copy or not
    else seg.stopTransition();
  }
  resume();
}

// wait until frame is over (service() has finished or time for 2 frames have passed; yield() crashes on 8266)
// the latter may, in rare circumstances, lead to incorrectly assuming strip is done servicing but will not block
// other processing "indefinitely"
// rare circumstances are: setting FPS to high number (i.e. 120) and have very slow effect that will need more
// time than 2 * _frametime (1000/FPS) to draw content
void WS2812FX::waitForIt() {
  unsigned long waitStart = millis();
  unsigned long maxWait = 2*getFrameTime() + 100; // TODO: this needs a proper fix for timeout! see #4779
  while (isServicing() && (millis() - waitStart < maxWait)) delay(1); // safe even when millis() rolls over
  #ifdef WLED_DEBUG
  if (millis()-waitStart >= maxWait) DEBUG_PRINTLN(F("Waited for strip to finish servicing."));
  #endif
};

void WS2812FX::setTargetFps(unsigned fps) {
  if (fps <= 250) _targetFps = fps;
  if (_targetFps > 0) _frametime = 1000 / _targetFps;
  else _frametime = MIN_FRAME_DELAY;     // unlimited mode
}

void WS2812FX::setCCT(uint16_t k) {
  for (Segment &seg : _segments) {
    if (seg.isActive() && seg.isSelected()) {
      seg.setCCT(k);
    }
  }
}

// direct=true either expects the caller to call show() themselves (realtime modes) or be ok waiting for the next frame for the change to apply
// direct=false immediately triggers an effect redraw
void WS2812FX::setBrightness(uint8_t b, bool direct) {
  setBrightness16((uint16_t)b << 8, direct);
}

void WS2812FX::setBrightness16(uint16_t b, bool direct) {
  const uint16_t maxBri16 = 255U << 8;
  if (b > maxBri16) b = maxBri16; // safety clamp just in case
  if (gammaCorrectBri && b > 0) {
    b = (int)(powf((float)b / maxBri16, gammaCorrectVal) * maxBri16 + 0.5f); // use float math not the gamma LUT to preserve 16bit precision
    if (b < (1U << 8)) b = 1U << 8; // prevent 0 brightness (after shifting) so LEDs do not turn off
  }
  if (_brightness16 == b) return;
  _brightness16 = b;
  if (_brightness16 == 0) { // unfreeze all segments on power off and stop all ongoing segment transitions
    for (Segment &seg : _segments) {
      seg.freeze = false; // freeze is mutable
      seg.stopTransition(); // stop transition, nothing to display anymore
    }
  }
  BusManager::setBrightness16(scaledBri16(b));
  if (!direct) {
    unsigned long t = millis();
    if (t - _lastShow > min(_frametime, uint16_t(FRAMETIME_FIXED))) trigger(); //apply brightness change immediately if no refresh soon, but don't speed up above 42fps
  }
}

uint8_t WS2812FX::getActiveSegsLightCapabilities(bool selectedOnly) const {
  uint8_t totalLC = 0;
  for (const Segment &seg : _segments) {
    if (seg.isActive() && (!selectedOnly || seg.isSelected())) totalLC |= seg.getLightCapabilities();
  }
  return totalLC;
}

uint8_t WS2812FX::getFirstSelectedSegId() const {
  size_t i = 0;
  for (const Segment &seg : _segments) {
    if (seg.isActive() && seg.isSelected()) return i;
    i++;
  }
  // if none selected, use the main segment
  return getMainSegmentId();
}

void WS2812FX::setMainSegmentId(unsigned n) {
  _mainSegment = getLastActiveSegmentId();
  if (n < _segments.size() && _segments[n].isActive()) {  // only set if segment is active
    _mainSegment = n;
  }
  return;
}

uint8_t WS2812FX::getLastActiveSegmentId() const {
  for (size_t i = _segments.size() -1; i > 0; i--) {
    if (_segments[i].isActive()) return i;
  }
  return 0;
}

uint8_t WS2812FX::getActiveSegmentsNum() const {
  unsigned c = 0;
  for (const Segment &seg : _segments) if (seg.isActive()) c++;
  return c;
}

uint16_t WS2812FX::getLengthTotal() const {
  unsigned len = Segment::maxWidth * Segment::maxHeight; // will be _length for 1D (see finalizeInit()) but should cover whole matrix for 2D
  if (isMatrix && _length > len) len = _length; // for 2D with trailing strip
  if (isMatrix && _logicalLength > len) len = _logicalLength; // sparse matrix ledmap with gaps and trailing strip (see deserializeMap())
  return len;
}

uint16_t WS2812FX::getLengthPhysical() const {
  return BusManager::getTotalLength(true);
}

//used for JSON API info.leds.rgbw. Little practical use, deprecate with info.leds.rgbw.
//returns if there is an RGBW bus (supports RGB and White, not only white)
//not influenced by auto-white mode, also true if white slider does not affect output white channel
bool WS2812FX::hasRGBWBus() const {
  for (size_t b = 0; b < BusManager::getNumBusses(); b++) {
    const Bus *bus = BusManager::getBus(b);
    if (!bus || !bus->isOk()) break;
    if (bus->hasRGB() && bus->hasWhite()) return true;
  }
  return false;
}

bool WS2812FX::hasCCTBus() const {
  if (cctFromRgb && !correctWB) return false;
  for (size_t b = 0; b < BusManager::getNumBusses(); b++) {
    const Bus *bus = BusManager::getBus(b);
    if (!bus || !bus->isOk()) break;
    if (bus->hasCCT()) return true;
  }
  return false;
}

void WS2812FX::purgeSegments() {
  // remove all inactive segments (from the back)
  int deleted = 0;
  if (_segments.size() <= 1) return;
  for (size_t i = _segments.size()-1; i > 0; i--)
    if (_segments[i].stop == 0) {
      deleted++;
      _segments.erase(_segments.begin() + i);
    }
  if (deleted) {
    _stateCacheValid = false;
    _segments.shrink_to_fit();
    setMainSegmentId(0);
  }
}

Segment& WS2812FX::getSegment(unsigned id) {
  return _segments[id >= _segments.size() ? getMainSegmentId() : id]; // vectors
}

// WARNING: resetSegments(), makeAutoSegments() and fixInvalidSegments() must not be called while
// strip is being serviced (strip.service()), you must call suspend prior if changing segments outside
// loop() context
void WS2812FX::resetSegments() {
  if (isServicing()) return;
  _stateCacheValid = false;
  _segments.clear();          // destructs all Segment as part of clearing
  _segments.emplace_back(0, isMatrix ? Segment::maxWidth : _length, 0, isMatrix ? Segment::maxHeight : 1);
  if (getActiveSegmentsNum() == 0) {
    _segments.clear();        // free failed segment
    _segments.emplace_back(); // if out of heap, create a default 30 pixel segment
    errorFlag = ERR_NORAM_PX;
  }
  _segments.shrink_to_fit();  // just in case ...
  _mainSegment = 0;
}

void WS2812FX::makeAutoSegments(bool forceReset) {
  if (isServicing()) return;
  _stateCacheValid = false;
  if (autoSegments) { //make one segment per bus
    unsigned segStarts[MAX_NUM_SEGMENTS] = {0};
    unsigned segStops [MAX_NUM_SEGMENTS] = {0};
    size_t s = 0;

    #ifndef WLED_DISABLE_2D
    // 2D segment is the 1st one using entire matrix
    if (isMatrix) {
      segStarts[0] = 0;
      segStops[0]  = Segment::maxWidth*Segment::maxHeight;
      s++;
    }
    #endif

    for (size_t i = s; i < BusManager::getNumBusses(); i++) {
      const Bus *bus = BusManager::getBus(i);
      if (!bus) break;

      segStarts[s] = bus->getStart();
      segStops[s]  = segStarts[s] + bus->getLength();

      #ifndef WLED_DISABLE_2D
      if (isMatrix && segStops[s] <= Segment::maxWidth*Segment::maxHeight) continue; // ignore buses comprising matrix
      if (isMatrix && segStarts[s] < Segment::maxWidth*Segment::maxHeight) segStarts[s] = Segment::maxWidth*Segment::maxHeight;
      #endif

      //check for overlap with previous segments
      for (size_t j = 0; j < s; j++) {
        if (segStops[j] > segStarts[s] && segStarts[j] < segStops[s]) {
          //segments overlap, merge
          segStarts[j] = min(segStarts[s],segStarts[j]);
          segStops [j] = max(segStops [s],segStops [j]); segStops[s] = 0;
          s--;
        }
      }
      s++;
    }

    _segments.clear();
    _segments.reserve(s); // prevent reallocations
    // there is always at least one segment (but we need to differentiate between 1D and 2D)
    #ifndef WLED_DISABLE_2D
    if (isMatrix)
      _segments.emplace_back(0, Segment::maxWidth, 0, Segment::maxHeight);
    else
    #endif
      _segments.emplace_back(segStarts[0], segStops[0]);
    for (size_t i = 1; i < s; i++) {
      _segments.emplace_back(segStarts[i], segStops[i]);
    }
    for (size_t i = 0; i < _segments.size(); i++) {
      _segments[i].colors[0] = DEFAULT_COLOR; // set color to default orange on all segments
    }
    DEBUGFX_PRINTF_P(PSTR("%d auto segments created.\n"), _segments.size());

  } else {

    if (forceReset || getSegmentsNum() == 0) resetSegments();
    //expand the main seg to the entire length, but only if there are no other segments, or reset is forced
    else if (getActiveSegmentsNum() == 1) {
      size_t i = getLastActiveSegmentId();
      #ifndef WLED_DISABLE_2D
      _segments[i].setGeometry(0, Segment::maxWidth, 1, 0, 0xFFFF, 0, Segment::maxHeight);
      #else
      _segments[i].setGeometry(0, _length);
      #endif
    }
  }
  if (getActiveSegmentsNum() == 0) resetSegments(); // fallback if auto segment creation failed
  _mainSegment = 0;

  fixInvalidSegments();
}

void WS2812FX::fixInvalidSegments() {
  if (isServicing()) return;
  _stateCacheValid = false;
  //make sure no segment is longer than total (sanity check)
  for (size_t i = getSegmentsNum()-1; i > 0; i--) {
    if (isMatrix) {
    #ifndef WLED_DISABLE_2D
      if (_segments[i].start >= Segment::maxWidth * Segment::maxHeight) {
        // 1D segment at the end of matrix (trailing strip; logical length may exceed physical _length for sparse matrix ledmaps)
        if (_segments[i].start >= getLengthTotal() || _segments[i].startY > 0 || _segments[i].stopY > 1) { _segments.erase(_segments.begin()+i); continue; }
        if (_segments[i].stop  >  getLengthTotal()) _segments[i].stop = getLengthTotal();
        continue;
      }
      if (_segments[i].start >= Segment::maxWidth || _segments[i].startY >= Segment::maxHeight) { _segments.erase(_segments.begin()+i); continue; }
      if (_segments[i].stop  >  Segment::maxWidth)  _segments[i].stop  = Segment::maxWidth;
      if (_segments[i].stopY >  Segment::maxHeight) _segments[i].stopY = Segment::maxHeight;
    #endif
    } else {
      if (_segments[i].start >= _length) { _segments.erase(_segments.begin()+i); continue; }
      if (_segments[i].stop  >  _length) _segments[i].stop = _length;
    }
  }
  // if any segments were deleted free memory
  purgeSegments();
  // this is always called as the last step after finalizeInit(), update covered bus types
  for (const Segment &seg : _segments)
    seg.refreshLightCapabilities();
}

//true if all segments align with a bus, or if a segment covers the total length
//irrelevant in 2D set-up
bool WS2812FX::checkSegmentAlignment() const {
  bool aligned = false;
  for (const Segment &seg : _segments) {
    for (unsigned b = 0; b<BusManager::getNumBusses(); b++) {
      const Bus *bus = BusManager::getBus(b);
      if (!bus || !bus->isOk()) break;
      if (seg.start == bus->getStart() && seg.stop == bus->getStart() + bus->getLength()) aligned = true;
    }
    if (seg.start == 0 && seg.stop == _length) aligned = true;
    if (!aligned) return false;
  }
  return true;
}

// used by analog clock overlay
void WS2812FX::setRange(uint16_t i, uint16_t i2, uint32_t col) {
  if (i2 < i) std::swap(i,i2);
  for (unsigned x = i; x <= i2; x++) setPixelColor(x, col);
}

#ifdef WLED_DEBUG
void WS2812FX::printSize() {
  size_t size = 0;
  for (const Segment &seg : _segments) size += seg.getSize();
  DEBUG_PRINTF_P(PSTR("Segments: %d -> %u/%dB\n"), _segments.size(), size, Segment::getUsedSegmentData());
  for (const Segment &seg : _segments) DEBUG_PRINTF_P(PSTR("  Seg: %d,%d [A=%d, 2D=%d, RGB=%d, W=%d, CCT=%d]\n"), seg.width(), seg.height(), seg.isActive(), seg.is2D(), seg.hasRGB(), seg.hasWhite(), seg.isCCT());
  DEBUG_PRINTF_P(PSTR("Modes: %d*%d=%uB\n"), sizeof(mode_ptr), _mode.size(), (_mode.capacity()*sizeof(mode_ptr)));
  DEBUG_PRINTF_P(PSTR("Data: %d*%d=%uB\n"), sizeof(const char *), _modeData.size(), (_modeData.capacity()*sizeof(const char *)));
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
  const uint32_t physMapBytesPerPixel = sizeof(*_physOwner) + sizeof(*_physIdx);
  DEBUG_PRINTF_P(PSTR("PhysMap: %u*%d=%uB\n"), physMapBytesPerPixel, (int)_physMapLen, _physMapLen*physMapBytesPerPixel);
#else
  DEBUG_PRINTF_P(PSTR("InvMap: %d*%d=%uB\n"), sizeof(uint16_t), (int)_invMappingSize, _invMappingSize*sizeof(uint16_t));
#endif
}
#endif

// Helper to build _invMappingTable[_length] directly from /ledmap[n].json (no forward table needed)
bool WS2812FX::loadInvMapFromFile(unsigned n) {
  char fileName[32];
  strcpy_P(fileName, PSTR("/ledmap"));
  if (n) sprintf(fileName + 7, "%d", n);
  strcat_P(fileName, PSTR(".json"));
  if (!WLED_FS.exists(fileName)) return false;

  d_free(_invMappingTable);
  _invMappingTable = static_cast<uint16_t*>(d_malloc(sizeof(uint16_t) * _length));
  if (!_invMappingTable) {
    _invMappingSize = 0;
    DEBUG_PRINTLN(F("ERROR LED invMap allocation error."));
    return false;
  }
  _invMappingSize = _length;
  memset(_invMappingTable, 0xFF, sizeof(uint16_t) * _length);

  File f = WLED_FS.open(fileName, "r");
  if (f && f.find("\"map\"")) {
    int value;
    unsigned logIdx = 0;
    const unsigned maxEntries = isMatrix ? getLengthTotal() : _length;
    while (logIdx < maxEntries && readNextIntFromFile(f, value)) {
      if (value >= 0 && value < (int)_length) {
        _invMappingTable[value] = (uint16_t)logIdx;
      }
      logIdx++;
    }
    f.close();
    return true;
  }
  if (f) f.close();
  return false;
}

// Helper to load forward customMappingTable transiently during realtimeMode
bool WS2812FX::loadForwardMapFromFile(unsigned n) {
  d_free(customMappingTable);
  customMappingTable = nullptr;
  customMappingSize = 0;

#ifndef WLED_DISABLE_2D
  if (!_hasCustomFileMap && _hasPanelGaps && isMatrix) {
    // Reconstruct forward map from panel[] + /2d-gaps.json
    const unsigned mapSize = getLengthTotal();
    customMappingTable = static_cast<uint16_t*>(d_malloc(sizeof(uint16_t) * mapSize));
    if (!customMappingTable) return false;
    customMappingSize = mapSize;
    const unsigned matrixSize = (unsigned)Segment::maxWidth * Segment::maxHeight;
    for (unsigned i = 0; i < matrixSize; i++) customMappingTable[i] = 0xFFFFU;
    for (unsigned i = matrixSize; i < mapSize; i++) customMappingTable[i] = i;
    buildPanelGapMap(customMappingTable, mapSize, true);
    return true;
  }
#endif

  char fileName[32];
  strcpy_P(fileName, PSTR("/ledmap"));
  if (n) sprintf(fileName + 7, "%d", n);
  strcat_P(fileName, PSTR(".json"));
  if (!WLED_FS.exists(fileName)) return false;

  const unsigned mapSize = isMatrix ? getLengthTotal() : _length;
  customMappingTable = static_cast<uint16_t*>(d_malloc(sizeof(uint16_t) * mapSize));
  if (!customMappingTable) return false;
  memset(customMappingTable, 0xFF, sizeof(uint16_t) * mapSize);
  File f = WLED_FS.open(fileName, "r");
  if (f && f.find("\"map\"")) {
    int value;
    unsigned mapindex = 0;
    while (mapindex < mapSize && readNextIntFromFile(f, value)) {
      if (value < 0 || value >= (int)_length) value = 0xFFFF;
      customMappingTable[mapindex++] = (uint16_t)value;
    }
    customMappingSize = mapSize;
    f.close();
    return true;
  }
  if (f) f.close();
  d_free(customMappingTable);
  customMappingTable = nullptr;
  return false;
}

// load custom mapping table from JSON file (called from finalizeInit() or deserializeState())
// if this is a matrix set-up and default ledmap.json file does not exist, create mapping from panel information
// WARNING: effect drawing has to be suspended (strip.suspend()) or must be called from loop() context
bool WS2812FX::deserializeMap(unsigned n) {
  char fileName[32];
  strcpy_P(fileName, PSTR("/ledmap"));
  if (n) sprintf(fileName + 7, "%d", n);
  strcat_P(fileName, PSTR(".json"));
  bool isFile = WLED_FS.exists(fileName);

  d_free(customMappingTable);
  customMappingTable = nullptr;
  customMappingSize = 0;
  d_free(_invMappingTable);
  _invMappingTable = nullptr;
  _invMappingSize = 0;
  _logicalLength = 0;
  _hasCustomFileMap = false;
  _hasPanelGaps = false;
  currentLedmap = 0;
  if (n == 0 || isFile) interfaceUpdateCallMode = CALL_MODE_WS_SEND; // schedule WS update (to inform UI)
  uint32_t lengthTotalBefore = strip.getLengthTotal();

  if (!isFile && n == 0 && isMatrix) {
    // 2D panel support creates its own mapping if a ledmap.json does not exist
    setUpMatrix();
    if (strip.getLengthTotal() != lengthTotalBefore)
      strip.updatePixelBuffer();
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    _physMapHash = 0;
#endif
    return false;
  }

  if (!isFile || !requestJSONBufferLock(JSON_LOCK_LEDMAP)) {
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
    _physMapHash = 0;
#endif
    return false;
  }

  StaticJsonDocument<64> filter;
  filter[F("width")]  = true;
  filter[F("height")] = true;
  if (!readObjectFromFile(fileName, nullptr, pDoc, &filter)) {
    DEBUG_PRINTF_P(PSTR("ERROR Invalid ledmap in %s\n"), fileName);
    releaseJSONBufferLock();
    return false; // if file does not load properly then exit
  } else
    DEBUG_PRINTF_P(PSTR("Reading LED map from %s\n"), fileName);

  JsonObject root = pDoc->as<JsonObject>();
  if (n == 0 && (!root[F("width")].isNull() || !root[F("height")].isNull())) {
    Segment::maxWidth  = min(max(root[F("width")].as<int>(), 1), 255);
    Segment::maxHeight = min(max(root[F("height")].as<int>(), 1), 255);
    isMatrix = true;
    DEBUG_PRINTF_P(PSTR("LED map width=%d, height=%d\n"), Segment::maxWidth, Segment::maxHeight);
  }
  releaseJSONBufferLock();

  if (isMatrix) {
    const unsigned matrixSize = Segment::maxWidth * Segment::maxHeight;
    unsigned entries = 0;
    unsigned numPhy = 0;
    File f = WLED_FS.open(fileName, "r");
    if (f && f.find("\"map\"")) {
      int value;
      while (entries++ < matrixSize && readNextIntFromFile(f, value)) {
        if (value >= 0 && value < (int)_length) numPhy++;
      }
    }
    if (f) f.close();
    if (entries > 0) {
      const unsigned trailingCount = (_length > numPhy) ? _length - numPhy : 0;
      _logicalLength = matrixSize + trailingCount;
    }
  } else {
    _logicalLength = _length;
  }

  if (_logicalLength > 0 && loadInvMapFromFile(n)) {
    _hasCustomFileMap = true;
    currentLedmap = n;
    #ifdef WLED_DEBUG
    DEBUG_PRINT(F("Loaded invMap:"));
    for (unsigned i = 0; i < _invMappingSize; i++) {
      if (!(i % Segment::maxWidth)) DEBUG_PRINTLN();
      DEBUG_PRINTF_P(PSTR("%4d,"), _invMappingTable[i] < 0xFFFFU ? _invMappingTable[i] : -1);
    }
    DEBUG_PRINTLN();
    #endif
  }

  if (strip.getLengthTotal() != lengthTotalBefore)
    strip.updatePixelBuffer();
#ifndef WLED_DISABLE_GLOBAL_PIXELBUFFER
  _physMapHash = 0;
  ensurePhysMapValid(); // builds _physMap and releases transient _invMappingTable
#endif
  return _hasCustomFileMap;
}


const char JSON_mode_names[] PROGMEM = R"=====(["FX names moved"])=====";
const char JSON_palette_names[] PROGMEM = R"=====([
"Default","* Random Cycle","* Color 1","* Colors 1&2","* Color Gradient","* Colors Only","Party","Cloud","Lava","Ocean",
"Forest","Rainbow","Rainbow Bands","Sunset","Rivendell","Breeze","Red & Blue","Yellowout","Analogous","Splash",
"Pastel","Sunset 2","Beach","Vintage","Departure","Landscape","Beech","Sherbet","Hult","Hult 64",
"Drywet","Jul","Grintage","Rewhi","Tertiary","Fire","Icefire","Cyane","Light Pink","Autumn",
"Magenta","Magred","Yelmag","Yelblu","Orange & Teal","Tiamat","April Night","Orangery","C9","Sakura",
"Aurora","Atlantica","C9 2","C9 New","Temperature","Aurora 2","Retro Clown","Candy","Toxy Reaf","Fairy Reaf",
"Semi Blue","Pink Candy","Red Reaf","Aqua Flash","Yelblu Hot","Lite Light","Red Flash","Blink Red","Red Shift","Red Tide",
"Candy2","Traffic Light","Fireworks"
])=====";
