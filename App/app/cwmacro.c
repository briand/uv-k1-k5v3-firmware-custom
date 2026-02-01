// CW Macro system implementation

#include "app/cwmacro.h"
#include "driver/eeprom.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "ui/main.h"
#include <string.h>

// Debug logging control - set to 1 to enable encoder debug output
#define CW_ENCODER_DEBUG 0

// Macro save/load debug logging - set to 1 to enable
#define CW_MACRO_DEBUG 1

// Morse code lookup table
// Pattern: LSB first, 0=dit, 1=dah
typedef struct {
	char ch;
	uint8_t length;   // Number of elements (1-6)
	uint8_t pattern;  // Bit pattern: 0=dit, 1=dah, LSB first
} MorseCode_t;

static const MorseCode_t MORSE_TABLE[] = {
	// Letters A-Z (LSB first: bit 0 = first element, 0=dit, 1=dah)
	{'A', 2, 0b10},      // .-
	{'B', 4, 0b0001},    // -...
	{'C', 4, 0b0101},    // -.-.
	{'D', 3, 0b001},     // -..
	{'E', 1, 0b0},       // .
	{'F', 4, 0b0100},    // ..-.
	{'G', 3, 0b011},     // --.
	{'H', 4, 0b0000},       // ....
	{'I', 2, 0b00},       // ..
	{'J', 4, 0b1110},    // .---
	{'K', 3, 0b101},     // -.-
	{'L', 4, 0b0010},    // .-..
	{'M', 2, 0b11},      // --
	{'N', 2, 0b01},      // -.
	{'O', 3, 0b111},     // ---
	{'P', 4, 0b0110},    // .--.
	{'Q', 4, 0b1011},    // --.-
	{'R', 3, 0b010},     // .-.
	{'S', 3, 0b000},       // ...
	{'T', 1, 0b1},       // -
	{'U', 3, 0b100},     // ..-
	{'V', 4, 0b1000},    // ...-
	{'W', 3, 0b110},     // .--
	{'X', 4, 0b1001},    // -..-
	{'Y', 4, 0b1101},    // -.--
	{'Z', 4, 0b0011},    // --..
	// Numbers 0-9
	{'0', 5, 0b11111},   // -----
	{'1', 5, 0b11110},   // .----
	{'2', 5, 0b11100},   // ..---
	{'3', 5, 0b11000},   // ...--
	{'4', 5, 0b10000},   // ....-
	{'5', 5, 0b00000},       // .....
	{'6', 5, 0b00001},       // -....
	{'7', 5, 0b00011},      // --...
	{'8', 5, 0b00111},     // ---..
	{'9', 5, 0b01111},    // ----.
	// Punctuation
	{'/', 5, 0b01001},    // -..-.
	{'?', 6, 0b001100},    // ..--..
};

#define MORSE_TABLE_SIZE (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

// Encoder state machine
static uint8_t s_encoder_pattern = 0;  // Accumulated pattern
static uint8_t s_encoder_length = 0;   // Number of elements in pattern
static bool s_encoder_space_pending = false;  // Word space before next char

// Recording state
bool gCW_Recording = false;
uint8_t gCW_RecordMacroIndex = 0;
uint8_t gCW_RecordBuffer[CW_MACRO_MAX_LEN];
uint8_t gCW_RecordLength = 0;
bool gCW_RecordNewChar = false;
// TX display buffer - shows characters being transmitted
char gCW_TX_Display[CW_TX_DISPLAY_SIZE];
uint8_t gCW_TX_DisplayIndex = 0;
bool gCW_TX_DisplayUpdated = false;
static const uint16_t MACRO_ADDRS[CW_MACRO_COUNT] = {
	CW_MACRO1_EEPROM_ADDR,
	CW_MACRO2_EEPROM_ADDR
};

bool CW_ValidateChar(char ch)
{
	// Valid characters: A-Z, 0-9, '/', '?'
	if (ch >= 'A' && ch <= 'Z')
		return true;
	if (ch >= '/' && ch <= '9')  // '/' to '9' are contiguous in ASCII
		return true;
	if (ch == '?')
		return true;
	return false;
}

uint8_t compute_macro_checksum(const uint8_t *block, uint8_t length)
{
	uint8_t sum = 0;
	// Sum payload bytes only (block[1] .. block[length])
	for (uint8_t i = 1; i <= length; i++) {
		sum += block[i];
	}
	return sum;
}

uint8_t CW_GetMacroLength(uint8_t macroIndex)
{
	if (macroIndex >= CW_MACRO_COUNT)
		return 0;
	
	uint8_t raw_len;
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex], &raw_len, 1);

#if CW_MACRO_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_GetMacroLength: idx=%u raw_len=0x%02x\r\n", macroIndex, raw_len);
		UART_Send(buf, strlen(buf));
	}
#endif

	// 0xFF = empty block
	if (raw_len == 0xFF)
		return 0;

	// Check signature bit (high bit set indicates a valid macro)
	if ((raw_len & CW_MACRO_SIG) == 0)
		return 0;

	uint8_t length = raw_len & ~CW_MACRO_SIG;
	if (length == 0 || length > CW_MACRO_MAX_LEN)
		return 0;

	// Validate checksum (read entire block including checksum byte)
	uint8_t block[CW_MACRO_BLOCK_SIZE];
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex], block, CW_MACRO_BLOCK_SIZE);
	uint8_t checksum = block[CW_MACRO_CHECKSUM_OFFSET];
	if (checksum != compute_macro_checksum(block, length))
		return 0;

	return length;
}

uint8_t CW_LoadMacro(uint8_t macroIndex, char *buffer, uint8_t bufferSize)
{
	if (macroIndex >= CW_MACRO_COUNT || buffer == NULL || bufferSize == 0)
		return 0;
	
	uint8_t length = CW_GetMacroLength(macroIndex);
	if (length == 0) {
		buffer[0] = '\0';
		return 0;
	}
	
	// Read the macro data block (length + payload + checksum)
	uint8_t block[CW_MACRO_BLOCK_SIZE];
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex], block, CW_MACRO_BLOCK_SIZE);

#if CW_MACRO_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_LoadMacro: read %u bytes from EEPROM (payload length=%u)\r\n", CW_MACRO_BLOCK_SIZE, length);
		UART_Send(buf, strlen(buf));
		for (uint8_t i = 0; i < length && i < 10; i++) {
			sprintf_(buf, "  [%u]=0x%02x '%c'%s\r\n", i, block[i+1], 
				CW_MACRO_GET_CHAR(block[i+1]), CW_MACRO_HAS_SPACE(block[i+1]) ? " +SPC" : "");
			UART_Send(buf, strlen(buf));
		}
	}
#endif
	
	// Decode into buffer with spaces
	uint8_t outPos = 0;
	for (uint8_t i = 0; i < length && outPos < bufferSize - 1; i++) {
		// Check if space precedes this character
		if (CW_MACRO_HAS_SPACE(block[i+1])) {
			if (outPos < bufferSize - 1) {
				buffer[outPos++] = ' ';
			}
		}
		
		// Get the character
		char ch = CW_MACRO_GET_CHAR(block[i+1]);
		if (outPos < bufferSize - 1) {
			buffer[outPos++] = ch;
		}
	}
	
	buffer[outPos] = '\0';
	return length;  // Return character count (not including spaces)
}

void CW_SaveMacro(uint8_t macroIndex, const char *buffer, uint8_t length)
{
	if (macroIndex >= CW_MACRO_COUNT || buffer == NULL)
		return;
	
	// Sanity check length
	if (length > CW_MACRO_MAX_LEN)
		length = CW_MACRO_MAX_LEN;

#if CW_MACRO_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_SaveMacro: idx=%u len=%u\r\n", macroIndex, length);
		UART_Send(buf, strlen(buf));
		// Show all bytes being saved
		for (uint8_t i = 0; i < length && i < 20; i++) {
			sprintf_(buf, "  [%u]=0x%02x (%c%s)\r\n", i, (uint8_t)buffer[i], 
				CW_MACRO_GET_CHAR(buffer[i]), CW_MACRO_HAS_SPACE(buffer[i]) ? " +SPC" : "");
			UART_Send(buf, strlen(buf));
		}
		if (length > 20) {
			sprintf_(buf, "  ... and %u more\r\n", length - 20);
			UART_Send(buf, strlen(buf));
		}
	}
#endif

	// Prepare data to write (CW_MACRO_BLOCK_SIZE bytes total)
	uint8_t data[CW_MACRO_BLOCK_SIZE];
	memset(data, 0xFF, sizeof(data));
	
	// First byte is the character count with signature bit (or 0xFF for empty)
	data[0] = (length == 0) ? 0xFF : ((length & ~CW_MACRO_SIG) | CW_MACRO_SIG);
	
	// Copy encoded characters from buffer (payload up to CW_MACRO_MAX_LEN)
	for (uint8_t i = 0; i < length; i++) {
		data[i + 1] = buffer[i];
	}

	// Compute and store checksum in the extra byte after payload
	data[CW_MACRO_CHECKSUM_OFFSET] = compute_macro_checksum(data, length);

#if CW_MACRO_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_SaveMacro: prepared %u-byte EEPROM block\r\n", (uint8_t)sizeof(data));
		UART_Send(buf, strlen(buf));
		sprintf_(buf, "  data[0]=0x%02x (length byte)\r\n", data[0]);
		UART_Send(buf, strlen(buf));
		for (uint8_t i = 1; i <= length && i <= 20; i++) {
			sprintf_(buf, "  data[%u]=0x%02x\r\n", i, data[i]);
			UART_Send(buf, strlen(buf));
		}
	}
#endif

	// Write entire 40-byte block to EEPROM in 8-byte chunks
	// EEPROM_WriteBuffer only writes 8 bytes at a time
	for (uint8_t i = 0; i < CW_MACRO_BLOCK_SIZE; i += 8) {
		EEPROM_WriteBuffer(MACRO_ADDRS[macroIndex] + i, data + i);
	}
}

uint8_t CW_FormatMacroDisplay(uint8_t macroIndex, char *display, uint8_t maxChars)
{
	if (display == NULL || maxChars == 0)
		return 0;
	
	// Use CW_GetMacroLength to validate signature + checksum
	uint8_t length = CW_GetMacroLength(macroIndex);
	if (length == 0) {
		strcpy(display, "empty");
		return 0;
	}

	// Read first up to 9 encoded bytes (start after length byte)
	uint8_t data[10];
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex] + 1, data, (length < 9) ? (length) : 9);

	// Copy up to 9 display positions (spaces + chars)
	uint8_t outPos = 0;
	uint8_t dispCount = 0;

	for (uint8_t i = 0; i < length && i < 9 && dispCount < 9; i++) {
		// Add space if present
		if (CW_MACRO_HAS_SPACE(data[i]) && dispCount < 9) {
			display[outPos++] = ' ';
			dispCount++;
		}
		// Add character if room
		if (dispCount < 9) {
			display[outPos++] = CW_MACRO_GET_CHAR(data[i]);
			dispCount++;
		}
	}

	// Add newline and char count
	outPos += sprintf_(display + outPos, "\n%u chars", length);

	return outPos;
}

// Decode accumulated pattern into a character
static char CW_DecodePattern(uint8_t pattern, uint8_t length)
{
	if (length == 0 || length > 6)
		return 0;
	
	for (unsigned int i = 0; i < MORSE_TABLE_SIZE; i++) {
		if (MORSE_TABLE[i].length == length && MORSE_TABLE[i].pattern == pattern) {
			return MORSE_TABLE[i].ch;
		}
	}
	
	return 0;  // Unknown pattern
}

// Playback helper: lookup pattern/length for a character
bool CW_GetMorseForChar(char ch, uint8_t *pattern, uint8_t *length)
{
	if (pattern == NULL || length == NULL)
		return false;

	for (unsigned int i = 0; i < MORSE_TABLE_SIZE; i++) {
		if (MORSE_TABLE[i].ch == ch) {
			*pattern = MORSE_TABLE[i].pattern;
			*length = MORSE_TABLE[i].length;
			return true;
		}
	}
	return false;
}

void CW_EncoderProcessElement(CW_ElementType_t element)
{
#if CW_ENCODER_DEBUG
	const char* elem_names[] = {"DIT", "DAH", "INTER_CHAR", "INTER_WORD"};
	if (element <= CW_ELEMENT_INTER_WORD_SPACE) {
		char buf[32];
		sprintf_(buf, "ENC: %s\r\n", elem_names[element]);
		UART_Send(buf, strlen(buf));
	}
#endif

	switch (element) {
	case CW_ELEMENT_DIT:
		// Add a dit (0) to the pattern
		if (s_encoder_length < 6) {
			// Pattern LSB first, so no shift needed for new bit
			s_encoder_length++;
		}
		break;
		
	case CW_ELEMENT_DAH:
		// Add a dah (1) to the pattern
		if (s_encoder_length < 6) {
			s_encoder_pattern |= (1 << s_encoder_length);
			s_encoder_length++;
		}
		break;
		
	case CW_ELEMENT_INTER_CHAR_SPACE:
		// End of character - decode and emit
#if CW_ENCODER_DEBUG
		{
			char buf[64];
			sprintf_(buf, "DECODE: pattern=0x%02x len=%d\r\n", s_encoder_pattern, s_encoder_length);
			UART_Send(buf, strlen(buf));
		}
#endif
		if (s_encoder_length > 0) {
			char ch = CW_DecodePattern(s_encoder_pattern, s_encoder_length);
#if CW_ENCODER_DEBUG
			{
				char buf[64];
				sprintf_(buf, "  Result: ch='%c' (%d) valid=%d\r\n", 
					ch ? ch : '?', ch, ch != 0 && CW_ValidateChar(ch));
				UART_Send(buf, strlen(buf));
			}
#endif
			if (ch != 0 && CW_ValidateChar(ch)) {
#if CW_ENCODER_DEBUG
				// Print decoded character for debug
				char buf[32];
				if (s_encoder_space_pending) {
					sprintf_(buf, "CW: (space) [%c]\r\n", ch);
				} else {
					sprintf_(buf, "CW: [%c]\r\n", ch);
				}
				UART_Send(buf, strlen(buf));
#endif
				
				// Emit to recorder if recording
				if (gCW_Recording && gCW_RecordLength < CW_MACRO_MAX_LEN) {
					gCW_RecordBuffer[gCW_RecordLength++] = CW_MACRO_ENCODE(ch, s_encoder_space_pending);
					gCW_RecordNewChar = true;
				}
				// Add to TX display buffer when transmitting (not recording)
				else if (!gCW_Recording) {
					CW_AddToTxDisplay(ch, s_encoder_space_pending);
				}
			}
			
			// Reset for next character
			s_encoder_pattern = 0;
			s_encoder_length = 0;
			s_encoder_space_pending = false;
		}
		break;
		
	case CW_ELEMENT_INTER_WORD_SPACE:
		// Mark that next character should have space before it
		// Also reset encoder state for next character
		s_encoder_pattern = 0;
		s_encoder_length = 0;
		s_encoder_space_pending = true;
		break;
	}
}

// Recording functions
void CW_StartRecording(uint8_t macroIndex)
{
	if (macroIndex >= CW_MACRO_COUNT)
		return;
	
	gCW_RecordMacroIndex = macroIndex;
	gCW_RecordLength = 0;
	gCW_RecordNewChar = false;
	gCW_Recording = true;
	
	// Reset encoder state
	s_encoder_pattern = 0;
	s_encoder_length = 0;
	s_encoder_space_pending = false;
}

void CW_StopRecording(void)
{
	if (!gCW_Recording)
		return;
	
#if CW_MACRO_DEBUG
	char buf[64];
	sprintf_(buf, "CW_StopRecording: saving %u chars to macro %u\r\n", gCW_RecordLength, gCW_RecordMacroIndex);
	UART_Send(buf, strlen(buf));
#endif
	
	// Save the recorded macro
	CW_SaveMacro(gCW_RecordMacroIndex, (const char *)gCW_RecordBuffer, gCW_RecordLength);
	
#if CW_MACRO_DEBUG
	sprintf_(buf, "CW_StopRecording: save complete\r\n");
	UART_Send(buf, strlen(buf));
#endif
	
	gCW_Recording = false;
	gCW_RecordNewChar = false;
}

bool CW_IsRecording(void)
{
	return gCW_Recording;
}

void CW_AddToTxDisplay(char ch, bool hasSpace)
{
#if CW_ENCODER_DEBUG
	char buf[64];
	sprintf_(buf, "AddToTx: ch='%c' space=%d idx=%d\r\n", ch, hasSpace, gCW_TX_DisplayIndex);
	UART_Send(buf, strlen(buf));
#endif
	
	// Add space first if needed
	if (hasSpace) {
		if (gCW_TX_DisplayIndex >= CW_TX_DISPLAY_SIZE - 1) {
			// Buffer full, shift left by 1 to make room
			memmove(gCW_TX_Display, gCW_TX_Display + 1, CW_TX_DISPLAY_SIZE - 2);
			gCW_TX_DisplayIndex = CW_TX_DISPLAY_SIZE - 2;
		}
		gCW_TX_Display[gCW_TX_DisplayIndex++] = ' ';
		gCW_TX_Display[gCW_TX_DisplayIndex] = '\0';
	}
	
	// Add character
	if (gCW_TX_DisplayIndex >= CW_TX_DISPLAY_SIZE - 1) {
		// Buffer full, shift left by 1 to make room
		memmove(gCW_TX_Display, gCW_TX_Display + 1, CW_TX_DISPLAY_SIZE - 2);
		gCW_TX_DisplayIndex = CW_TX_DISPLAY_SIZE - 2;
	}
	gCW_TX_Display[gCW_TX_DisplayIndex++] = ch;
	gCW_TX_Display[gCW_TX_DisplayIndex] = '\0';
	
	// Trigger main display update
	gUpdateDisplay = true;
	
#if CW_ENCODER_DEBUG
	sprintf_(buf, "  Buffer now: \"%s\" (len=%d)\r\n", gCW_TX_Display, strlen(gCW_TX_Display));
	UART_Send(buf, strlen(buf));
#endif
}

void CW_ClearTxDisplay(void)
{
#if CW_ENCODER_DEBUG
	char buf[64];
	sprintf_(buf, "ClearTxDisplay called\r\n");
	UART_Send(buf, strlen(buf));
#endif
	memset(gCW_TX_Display, 0, CW_TX_DISPLAY_SIZE);
	gCW_TX_DisplayIndex = 0;
}

uint8_t CW_GetRecordingDisplay(char *display, uint8_t maxChars)
{
	if (display == NULL || maxChars == 0 || !gCW_Recording)
		return 0;
	
	uint8_t cursor_pos = 0;
	uint8_t outPos = 0;
	
	// Calculate total display positions (each space and each char = 1 position)
	uint8_t total_positions = 0;
	for (uint8_t i = 0; i < gCW_RecordLength; i++) {
		if (CW_MACRO_HAS_SPACE(gCW_RecordBuffer[i])) {
			total_positions++;  // Space position
		}
		total_positions++;  // Character position
	}
	
	// Calculate how many positions to skip from the left (scroll when > 9)
	uint8_t skip_positions = 0;
	if (total_positions > 9) {
		skip_positions = total_positions - 9;
	}
	
	// Build display string, skipping positions from the left
	uint8_t position_count = 0;
	for (uint8_t i = 0; i < gCW_RecordLength && outPos < maxChars - 2; i++) {
		// Handle space before character
		if (CW_MACRO_HAS_SPACE(gCW_RecordBuffer[i])) {
			if (position_count >= skip_positions) {
				// This space position is visible
				if (outPos < maxChars - 2) {
					display[outPos++] = ' ';
				}
			}
			position_count++;
		}
		
		// Handle character
		if (position_count >= skip_positions) {
			// This character position is visible
			char ch = CW_MACRO_GET_CHAR(gCW_RecordBuffer[i]);
			if (outPos < maxChars - 2) {
				display[outPos++] = ch;
			}
		}
		position_count++;
	}
	
	// Add cursor placeholder if room, or space if buffer is full
	if (outPos < maxChars - 1) {
		if (gCW_RecordLength < CW_MACRO_MAX_LEN) {
			display[outPos++] = '_';
		} else {
			display[outPos++] = ' ';  // Space to maintain alignment when full
		}
	}
	
	// Cursor position is at the underscore (currently at outPos - 1)
	if (outPos > 0 && display[outPos - 1] == '_') {
		cursor_pos = outPos - 1;
	} else {
		cursor_pos = outPos;
	}

	// Pad with spaces up to 10 chars to prevent centering jitter
	// Only pad if total content (before padding) is < 10 positions
	if (total_positions < 10) {
		while (outPos < 10 && outPos < maxChars - 1) {
			display[outPos++] = ' ';
		}
	}
	
	display[outPos] = '\0';
	
	return cursor_pos;
}
