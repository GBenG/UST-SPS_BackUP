/*
 * readme.c
 *
 *  Created on: 04.07.2012
 *      Author: d.sapunov
 */

#include "sdk_common.h"
#include "system.h"
#include "readme.h"
#include "symbols.h"
#include "screen.h"
#include "keyb.h"
#include "fifo.h"
#include "beep.h"
#include "printer.h"
#include "debug.h"
#include "localization/menu.h"
#include "LCDUI/supervisor.h"

void LCD_FlashReadmeWithButtons(char* text, eJustify justify, char* left_button, char* right_button){
	sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();

	if(window) {
		MUTEX_LOCK(window->mutex) {
			sScreen* screen = &window->screen;
			Screen_DrawFlashMessageWithButtons(screen, text, justify,left_button,right_button);
		}
		MUTEX_UNLOCK(window->mutex)
	}
}

void LCD_FlashReadme(char* text, eJustify justify) {
	LCD_FlashReadmeWithButtons(text, justify, NULL, NULL);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
eKey LCD_ReadmeWithTimeout(char* text, eJustify justify, char* left_button, char* right_button, unsigned long timeout_ms, eKey timeout_key) {

	sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();
	if(!window) {
		if(left_button) return KEY_LSOFT;
		else return KEY_RSOFT;
	}

	char* ptr_end = text + strlen(text);
	const int MAX_MARKS = 256; //brother of Carl //sps: ј еще это кол-во окон с текстом которые можно упихать в пам€ть дл€ отображени€
	char** bookmarks = LCDUI_malloc(MAX_MARKS*sizeof(char*));
	if (bookmarks == NULL) return KEY_NONE;
	bookmarks[0] = text;
	int marked = 1;
	int position = 0;
	bool need_redraw=true;

	eKey retval = KEY_NONE; unsigned long _timein_ms = systemGetTimer();
	for (;;) {
		char* ptr = NULL;
		if(need_redraw)
		{
			MUTEX_LOCK(window->mutex)
			{
				sScreen* screen = &window->screen;
				ptr = Screen_DrawFlashMessageWithButtons(screen, bookmarks[position], justify, left_button, right_button); //sps: вывод 6 строк на окно экрана
			}

			MUTEX_UNLOCK(window->mutex)
			need_redraw=false;

			if (position + 1 == marked && marked < MAX_MARKS && ptr != ptr_end)
			{
				bookmarks[marked++] = ptr;
			}
		}

		eKey key = LCDUI_Window_FetchKey(window);
		if (key != KEY_NONE)
		{
			if ((key == KEY_UP || key==KEY_PGUP) && position > 0)
			{
				position--;
			}
			else if ((key == KEY_DOWN || key == KEY_PGDOWN) && (ptr != ptr_end && position + 1 < marked))
			{
				position++;
			}
			else if (key == KEY_RSOFT && right_button != NULL) {
				retval = key;
				break;
			} else if (key == KEY_LSOFT && left_button != NULL) {
				retval = key;
				break;
			} else if (key == KEY_PRINT) {
				printMessage(text);
			} else {
				beepError();
			}
			need_redraw=true;
			continue;
		}

		if(timeout_ms && (systemGetTimer() - _timein_ms) >= timeout_ms) { retval = timeout_key; break; }

		taskYIELD();
	}

	LCDUI_free(bookmarks);
	return retval;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

eKey LCD_Readme(char* text, eJustify justify, char* left_button, char* right_button) {
	return LCD_ReadmeWithTimeout(text, justify, left_button, right_button, 0, KEY_NONE);
}

void LCD_ReadmeWithCancelEmptyButtonsWithTimeout(char* text, eJustify justify, unsigned long timeout_ms) { LCD_ReadmeWithTimeout(text, justify, LANG_MENU_BUTTON_CANCEL, NULL, timeout_ms, KEY_LSOFT); }
void LCD_ReadmeWithCancelEmptyButtons(char* text, eJustify justify) { LCD_ReadmeWithCancelEmptyButtonsWithTimeout(text, justify, 0); }

void LCD_ReadmeWithBackEmptyButtonsWithTimeout(char* text, eJustify justify, unsigned long timeout_ms) { LCD_ReadmeWithTimeout(text, justify, LANG_MENU_BUTTON_BACK, NULL, timeout_ms, KEY_LSOFT); }
void LCD_ReadmeWithBackEmptyButtons(char* text, eJustify justify) { LCD_ReadmeWithBackEmptyButtonsWithTimeout(text, justify, 0); }

void LCD_ReadmeWithEmptyNextButtonsWithTimeout(char* text, eJustify justify, unsigned long timeout_ms) { LCD_ReadmeWithTimeout(text, justify, NULL, LANG_MENU_BUTTON_NEXT, timeout_ms, KEY_RSOFT); }
void LCD_ReadmeWithEmptyNextButtons(char* text, eJustify justify) { LCD_ReadmeWithEmptyNextButtonsWithTimeout(text, justify, 0); }

void LCD_ReadmeWithEmptyOkButtonsWithTimeout(char* text, eJustify justify, unsigned long timeout_ms) { LCD_ReadmeWithTimeout(text, justify, NULL, LANG_MENU_BUTTON_OK, timeout_ms, KEY_RSOFT); }
void LCD_ReadmeWithEmptyOkButtons(char* text, eJustify justify) { LCD_ReadmeWithEmptyOkButtonsWithTimeout(text, justify, 0); }

void LCD_ReadmeWithCancelNextButtonsWithTimeout(char* text, eJustify justify, unsigned long timeout_ms) { LCD_ReadmeWithTimeout(text, justify, LANG_MENU_BUTTON_CANCEL, LANG_MENU_BUTTON_NEXT, timeout_ms, KEY_LSOFT); }
void LCD_ReadmeWithCancelNextButtons(char* text, eJustify justify) { LCD_ReadmeWithCancelNextButtonsWithTimeout(text, justify, 0); }


eKey LCD_ReadmeWithBackNextButtons(char* text, eJustify justify) {
	return LCD_Readme(text, justify, LANG_MENU_BUTTON_BACK, LANG_MENU_BUTTON_NEXT);
}

eKey LCD_ReadmeWithCancelOkButtons(char* text, eJustify justify) {
	return LCD_Readme(text, justify, LANG_MENU_BUTTON_CANCEL, LANG_MENU_BUTTON_OK);
}

eKey LCD_ReadmeWithNoYesButtons(char* text, eJustify justify) {
	return LCD_Readme(text, justify, LANG_MENU_BUTTON_NO, LANG_MENU_BUTTON_YES);
}

eKey LCD_ReadmeWithYesNoButtons(char* text, eJustify justify) {
	return LCD_Readme(text, justify, LANG_MENU_BUTTON_YES, LANG_MENU_BUTTON_NO);
}

eKey LCD_ReadmeWithStandardButtons(char* text, eJustify justify) {
	return LCD_ReadmeWithBackNextButtons(text, justify);
}

eKey LCD_ReadmeWithBackPrintButtons(char* text, eJustify justify) {
	return LCD_Readme(text, justify, LANG_MENU_BUTTON_BACK, LANG_MENU_BUTTON_PRINT);
}

bool LCD_AwaitingScreen(char* text, eJustify justify, bool (*ExitCondition)()) {
	if(ExitCondition && ExitCondition()) return true;

	sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();

	LCD_FlashReadmeWithButtons(text, justify, LANG_MENU_BUTTON_CANCEL,NULL);

	for (;;)
	{
		eKey key = LCDUI_Window_FetchKey(window);
		if (key != KEY_NONE)
		{
			if (key == KEY_LSOFT)
			{
				return false;
			}else if (key == KEY_OPL)
			{
				printMessage(text);
			}else { beepError(); }
		}

		if(ExitCondition && ExitCondition()) return true;

		taskYIELD();
	}

	return false;
}

void LCD_CustomDialog(bool (*idler)(eKey key)){
	if(!idler) return;
	sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();

	for (;;) {
		eKey key = LCDUI_Window_FetchKey(window);
		if(idler(key)) break;
		taskYIELD();
	}
}


