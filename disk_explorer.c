/*
 * disk_explorer.c
 *
 *  Created on: 31.01.2013
 *      Author: d.sapunov
 *  Update: 13.10.2013
 *      Coder: Suslov.P.S.
 */
#include "sdk_common.h"

/* Scheduler includes. */
#include <unistd.h>
#include "tasks_common.h"

#include <string.h>
#include "string2.h"

#include <ff.h>
#include "fat_ext/fat_ext.h"

#include "disk_explorer.h"
#include "LCDUI/dynamic_menu.h"
#include "LCDUI/window.h"
#include "LCDUI/readme.h"
#include "LCDUI/symbols.h"
#include "LCDUI/screen.h"		//sps
#include "LCDUI/messages.h"
#include "LCDUI/supervisor.h"
#include "LCDUI/forms/form.h"	//sps
#include "LCDUI/uawaits.h"		//sps
#include "debug.h"
#include "beep.h"
#include "localization/diskexplorer.h"
#include "localization/internet.h"
#include "localization/messages.h"
#include "localization/menu.h"
#include "printer.h"

#include "arraylist.h"
#ifdef FUNCTION_SOUND_BLASTER
#include "Sound/play_wav.h"
#endif
#include "update_fw.h"

static bool need_rebuild=false;

static void showFileInfo(FILINFO* fno, char* short_readable_name){
	char* msg=UNS_MALLOC(256);
	if(msg==NULL) return;

	char* ptr=msg;
	ptr+=sprintf(ptr,"%s\n",short_readable_name);
	ptr+=sprintf(ptr,LANG_DISKEXP_FILESIZE": %lu\n",fno->fsize);
	ptr+=sprintf(ptr,LANG_DISKEXP_PERMISSION":\n");
	ptr+=sprintf(ptr,"[%c]"LANG_DISKEXP_READING"\n",(fno->fattrib & AM_WRO)?' ':LCD_SYMBOL_BIRD);
	ptr+=sprintf(ptr,"[%c]"LANG_DISKEXP_WRITING"\n",(fno->fattrib & AM_RDO)?' ':LCD_SYMBOL_BIRD);

	LCD_ReadmeWithBackEmptyButtons(msg,JUSTIFY_NONE);
	UNS_FREE(msg);
}

static bool deleteFile(FILINFO* fno, char* full_path, char* short_readable_name){
	if(fno->fattrib & AM_RDO){
		beepError();
		toast_access_denied();
		return false;
	}

	char temp[80];
	sprintf(temp,"\"%s\"\n"LANG_DISKEXP_ASK_DELETE,short_readable_name);
	bool retval=false;
	eKey res=LCD_ReadmeWithNoYesButtons(temp,JUSTIFY_CENTER);
	if(res==KEY_RSOFT){
		FRESULT fres=f_unlink(full_path);
		if(fres!=FR_OK){
			toast_file_not_found(short_readable_name);
		}else{
			retval=true;
		}
	}

	return retval;
}

static void runFile(char* full_path, char* short_readable_name){
	if(stristr(short_readable_name,".ldr")){
		if(LCD_ReadmeWithNoYesButtons(LANG_MSG_ASK_UPDATE_FIRMWARE,JUSTIFY_CENTER)==KEY_RSOFT){
			applyFirmwareFromLDR(full_path, true, false);
		}
	}
	#ifdef FUNCTION_DEBUG
	else if(stristr(short_readable_name,".bin")){
		LCD_ReadmeWithBackEmptyButtons("BIN-файлы прошиваются только через SAMBA!\nОтсюда - только LDR!",JUSTIFY_CENTER);
	}
	#endif
	#ifdef FUNCTION_WAV_PLAYER
	else if(stristr(short_readable_name,".wav")){
		MediaPlayer(full_path,short_readable_name);	}
	#endif

	return;
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPS /// Подфункция проверки имен файлов //////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 2015 //
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static bool FileNameCheck(char* name)
{
	bool f_valex = false;	//sps: соответствие имени файла
	bool f_valnm = false;	//sps: соответствие имени папки
	bool f_valdn = false;	//sps: запрещенные в FAT символы
							// . " / \ [ ] : ; | = ,

	for(int i=0;i<strlen(name);i++)						//sps: Ищим расширение в имени, если его нет то это папка, сразу проверяем формат длинн
			{
				if(name[i]=='.' && i<=8 && (strlen(name)-i-1)<=3)
				{f_valex = true;};
				switch (name[i])						//sps: В случае нахождения запрещенных символов, обнуляем результат и уходим
		        {
		         case '"':	f_valdn = true;		break;
		         case '/': 	f_valdn = true;		break;
		         case '\\': f_valdn = true;		break;
		         case '[': 	f_valdn = true;		break;
		         case ']':	f_valdn = true;		break;
		         case ':': 	f_valdn = true;		break;
		         case ';': 	f_valdn = true;		break;
		         case '|': 	f_valdn = true;		break;
		         case '=': 	f_valdn = true;		break;
		         case ',':	f_valdn = true;		break;
		        }
			};

		if(f_valex!=true)													//sps: Это папка, проверим ее формат
	{
		if(strlen(name)<=8){f_valnm = true;}								//sps: Если имя папки < 8 символов - подходит!
	};

	if((f_valnm == true || f_valex == true) && f_valdn != true){
		DBG("FileNameCheck -> true")
		return true;}
	else{
		DBG("FileNameCheck -> false")
		LCDUI_Supervisor_Toast(LANG_DISKEXP_BADNAME,1500);					//sps: Неверный формат имени!
		return false;}
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPS ///  Подфункция копирования содержимого файла //////////////////////////////////////////////////////////////////////////////////////////////////////// 2015 //
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static FRESULT BufCopyFile(char* full_p, char* dup_p)
{
	FIL* fsrc=UNS_MALLOC_OBJ(FIL);					// файловые объект источник
	FIL* fdst=UNS_MALLOC_OBJ(FIL);      			// файловые объект	дубликат
	UINT nRead, nWritten, prgres;					//sps: Количество байт чтения / записи / прогресс
	FRESULT fres,dres;
	FRESULT res;
	char* buf; 										//sps: Буфер для записываемых данных
	buf=UNS_MALLOC(1024);							//sps: Выделим буферу память

	FILINFO fno;
	f_stat(full_p,&fno);
	UINT ffsize=fno.fsize;							//sps: UPD - Определяем размер исходного файла

	fres=f_open(fsrc,full_p,FA_READ);
	dres=f_open(fdst,dup_p, FA_CREATE_ALWAYS | FA_WRITE);

	if(fres==FR_OK && dres == FR_OK)
	{
		prgres=0;
		bool endProcess=false;												//sps: бнуляем прогресс
		bool procValue(int* newTimeStamp, void* data)
		{ *(int*)data=((prgres*100)/ffsize); return endProcess; }			//sps: Отображаем процесс дублирования
		void doDuplicate(void* params) {
			for (;;) {
				if (endProcess) { break; }
				fres=f_read(fsrc,buf,1024,&nRead);							//sps: Читаем содержимое в буфер
				if (fres || nRead == 0) break; 								//	   ошибка или eof (конец данных файла)
				else { dres=f_write(fdst, buf, nRead, &nWritten); }			//sps: Записываем содержимое из буфера
				if (dres || nWritten < nRead) break; 						// 	   ошибка, если диск переполнен
				else { prgres+=nRead; }										//sps: Рассчет прогресса дублирования
			}
			endProcess=true;
		}

		osTaskCreate(doDuplicate,"Core>Dup",200,NULL,TASK_PRIORITY_DEFAULT,false);
		eUASResult result=LCDUI_UniversalAwaitScreen(LANG_DISKEXP_STLCOPY,ehjCenter,euastPercent,euasbCancelOnly,0,procValue,NULL,NULL,NULL,NULL);

		if (result==euasrByUserLeft) {
			endProcess=true;
			f_close(fsrc);
			f_close(fdst);
			UNS_FREE(buf);
			res=FR_DENIED;
			return res;		//sps: "Операция дублирования ОМЕНА!"
		}
	}
f_close(fsrc);
f_close(fdst);
UNS_FREE(buf);
res=FR_OK;
return res;					//sps: "Операция дублирования ОК!"
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPS ///  Функция переименовывания файла с формой ввода и всплывающими окошками /////////////////////////////////////////////////////////////////////////// 2015 //
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void renameFile(FILINFO* fno, char* megapath,char* new_path){

	_LCDUI_Form* _form = LCDUI_Form_New();
	_LCDUI_TextField* _tf_text = LCDUI_TextField_New( "", INPUT_MODE_TEXT, INPUT_LAYOUT_DIGIT| INPUT_LAYOUT_LATIN, 512);
	 LCDUI_Form_AddLabeledControl(_form, LANG_DISKEXP_RENCAP"\n\n", _tf_text);
	_LCDUI_Action res = LCDUI_Form_Show(_form);

	char* ren_path=UNS_MALLOC(strlen(megapath)+32); 							//sps: Переменная для пути нового имени +32 символа под имя

	if(res == LCDUI_ACTION_OK)
	{
		LCDUI_Supervisor_Toast(_tf_text->text_buffer,1500);						//sps: Засветим на экран новое имя файла

		sprintf(ren_path,"%s/%s",megapath,_tf_text->text_buffer); 				//sps: Формирования нового имени с полным путем
		for(int i=0;i<strlen(ren_path);i++){ren_path[i]=ren_path[i+2];} 		//sps: Убираем из пути номер логического диска для ф-ции f_rename
//--------------------------------------------------------------------------------------------------------------------------------------------------------

		bool chknm=FileNameCheck(_tf_text->text_buffer);						//sps: Проверяем имя файла на соотвествие формату

//--------------------------------------------------------------------------------------------------------------------------------------------------------
		if(chknm==true)
		{
			FRESULT rres=f_rename(new_path,ren_path);							//sps: Переименовываем файл, возвращаем результат операции
			if(rres!=FR_OK)
			{
				DBGF("RESULT - %d ",rres)
				if(rres==FR_EXIST)												//sps: Файл с таким именем уже существует(
				{LCDUI_Supervisor_Toast(LANG_DISKEXP_FEXIST,1500);}
				if(rres==FR_INVALID_NAME || rres==FR_INVALID_OBJECT)
				{LCDUI_Supervisor_Toast(LANG_DISKEXP_BADNAME,1500);}			//sps: Неверный формат имени!
				if(rres==FR_NO_PATH)
				{LCDUI_Supervisor_Toast(LANG_DISKEXP_BADPATH,1500);}			//sps: Неверный формат пути!
				beep();
			}else{
				LCDUI_Supervisor_Toast(LANG_DISKEXP_RENAME,1000);				//sps: Все окей! Радуем пользователя веселым окошким
			};
		}
	};
	LCDUI_Form_Delete(_form);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPS ///  Функция дублирования файла ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////// 2015 //
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void duplicFile(FILINFO* fno, char* megapath, char* short_name)
{
	char* full_path=UNS_MALLOC(strlen(megapath)+13);
    char* dup_path=UNS_MALLOC(strlen(megapath)+13);

//-----------------------------------------------------------------------------------------------
    sprintf(full_path,"%s/%s",megapath,short_name);								//sps: формируем адрес исходного файла

    for(int i=0;i<strlen(short_name);i++)										//sps: сли расширение файла 1 символ, добавим подчеркивание
    {
    	if(short_name[i]=='.')
    	{
    		if((strlen(short_name)-i-1)<2)										//sps: Добаваляем последний символ имени дубликата "_"
			{sprintf(short_name,"%s_",short_name);
			DBGF("%s",short_name);
			}
    	}
    }
    short_name[strlen(short_name)-1]='_';										//sps: Меняем последний символ имени дубликата на "_"

//-----------------------------------------------------------------------------------------------
	_LCDUI_Form* _form = LCDUI_Form_New();
	_LCDUI_TextField* _tf_text = LCDUI_TextField_New( short_name, INPUT_MODE_TEXT, INPUT_LAYOUT_DIGIT| INPUT_LAYOUT_LATIN, 512);
	 LCDUI_Form_AddLabeledControl(_form, LANG_DISKEXP_RENCAP"\n\n", _tf_text);
//-----------------------------------------------------------------------------------------------
	 for(;;){
		_LCDUI_Action res = LCDUI_Form_Show(_form);
		FILINFO tfno;

		if(res == LCDUI_ACTION_OK)
		{

			bool chknm=FileNameCheck(_tf_text->text_buffer);					//sps: Проверяем имя файла на соотвествие формату

			if(chknm!=true)
			{
				LCDUI_Form_Delete(_form);
				return;
			}

			sprintf(dup_path,"%s/%s",megapath,_tf_text->text_buffer);			//sps: формируем адрес дубликата

			FRESULT fstat=f_stat(dup_path,&tfno);								//sps: Проверяем существует ли такой файл
			if(fstat==FR_OK)													//sps: Если файл с таким именем есть, спроим переписать?
			{
				eKey res=LCD_ReadmeWithNoYesButtons(LANG_DISKEXP_REWRASK,JUSTIFY_CENTER);
				if(res==KEY_RSOFT){break;}										//sps: Да, выходим из ожидания и перезаписываем
			}else{break;}														//sps: Такого файла не существует, смело пишем
		}else{
			LCDUI_Form_Delete(_form);											//sps: Ой все! Надоело, закроем окно и вернемся в меню
			return;
		}
	}
//-----------------------------------------------------------------------------------------------
	FRESULT  bcres=BufCopyFile(full_path, dup_path);							//sps: Функция буфферизированного копирования файла
	if(bcres==FR_OK){
		LCDUI_Supervisor_Toast(LANG_DISKEXP_DUPLIC,1500);						//sps: "Операция дублирования ОК!"
	}else{
		FILINFO dufno;
		f_stat(dup_path,&dufno);												//sps: Проверим параметры файла-огрызка
		LCDUI_Supervisor_Toast(LANG_DISKEXP_COPSTOP,1500);						//sps: "Операция дублирования ОМЕНА!"
		DBGF("Size of FILE = %d",fno->fsize);
		DBGF("Size of STUB = %d",dufno);
		if(dufno.fsize != fno->fsize){											//sps: Если размер прерванной копии не равен иходнику - сотрем огрызок
			DBG("Broken file is deleted [->X]");
			FRESULT delres=f_unlink(dup_path);
			if(delres!=FR_OK)LCDUI_Supervisor_Toast("Critical Error! File broken!",1500);
		}
	}
//-----------------------------------------------------------------------------------------------
	LCDUI_Form_Delete(_form);
};
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SPS ///  Функция чтения файла с переключателем видов [TXT]/[HEX] ///////////////////////////////////////////////////////////////////////////////////////// 2015 //
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static bool readFileBf(FILINFO* fno, char* full_path){
		#ifndef FUNCTION_DEBUG
		if(fno->fattrib & AM_WRO)
		{
			beepError();
			toast_access_denied();
			return false;
		}
		#endif
//-----------------------------------------------------------------------------------------------

		int    			point=0;				//sps: Позиция на которой сейчас отображаемый текст
		int    			cursor=0;				//sps: Позиция на которой сейчас курсор
		int    			size=fno->fsize;		//sps: Размер открываемого файла

//-----------------------------------------------------------------------------------------------

		#define wsize 1024														//sps:	Размер буфера
		#define hsize wsize/2													//sps:	Размер половины буфера
		#define scrsize LCD_CLIENT_WIDTH*LCD_CLIENT_HEIGHT						//sps:	Кол-во символов на экране в TXT
		#define hscrsze 20														//sps:	Кол-во символов на экране в HEX
		#define tstrsz 15														//sps:	Кол-во обрабатываемых символов в строке TXT
		#define hstrsz 4														//sps:	Кол-во обрабатываемых символов в строке HEX

//-----------------------------------------------------------------------------------------------
		char* 	wbuf=UNS_MALLOC(wsize+1);		//sps:	Буфера

		unsigned int len;						//sps:	Возврат прочитанных байт
		unsigned int offs=0;					//sps:	Смещение буфера по файлу
		unsigned int grab;						//sps:	Кол-во загружаеммых в буфер байт


//-----------------------------------------------------------------------------------------------
//================================================================================================
			void ReGrab(void)
			{
					FIL 	fil;
					FRESULT fres=f_open(&fil,full_path,FA_READ);	//sps:	Открываем файл

		/*			if(fres==FR_OK)
					{

						if(((fno->fsize)-offs)>=wsize){grab=wsize;}else{grab=((fno->fsize)-offs);}		//sps:	Вычисление кол-ва считываемых байт, чтобы не влезть за EOF

						memset(wbuf,0,wsize);						//sps:	Чистим буфер

						f_lseek(&fil, offs);						//sps:	Сдвигаем позицию считывания

						fres=f_read(&fil,wbuf,grab,&len);			//sps:	Читаем из файла
						sprintf(wbuf,"%s\0",wbuf);					//sps:	Затыкаем строку в буфере
					}
		 */
					if(fres==FR_OK)
					{
																			//sps:	Вычисление кол-ва считываемых байт, чтобы не влезть за EOF не забыть полностью загрузить первый буфер
						if(((fno->fsize)-offs)>=wsize){
							if(offs!=0){grab=hsize;}else{grab=wsize;}
						}else{
							grab=((fno->fsize)-offs);
						}

						memcpy(wbuf,wbuf+hsize,hsize);						//sps:	Переносим нижние пол буфера вверх

						f_lseek(&fil, offs);								//sps:	Сдвигаем позицию считывания

						if(offs!=0){
							fres=f_read(&fil,wbuf+hsize,grab,&len);			//sps:	Читаем из файла пол буфера
						}else{
							fres=f_read(&fil,wbuf,grab,&len);				//sps:	Читаем из файла целый буфер
						}
						sprintf(wbuf,"%s\0",wbuf);							//sps:	Затыкаем строку в буфере
					}

						f_close(&fil);										//sps:	Закрываем файл

		/*			DBG("Buffer RELOAD >>>");
					DBG("======================");
					DBGF("Buf_start => %d",offs);
					DBGF("Buf_end => %d",offs+len);
					DBGF("Point_now => %d",point);
					DBGF("Global_point => %d",offs+point);
					DBGF("Offset => %d",offs);
					DBGF("Wsize => %d",wsize);
					DBGF("Hsize => %d",hsize);
//					DBGF("|%s|",wbuf);
					DBG("======================");	*/
			}
//================================================================================================
//================================================================================================ SPS :: HexViewer
			eKey HexView(sLCDUI_Window* window) {

				sScreen* screen = &window->screen;

				bool 	need_redraw=true;										//sps: Пнуть в ТРУ если нужно перисовать окошко
				char*  	offset=UNS_MALLOC(tstrsz+1);							//sps: Смещение номера считанного байта файла в НЕХ
				char*  	hexstr=UNS_MALLOC(tstrsz+1);							//sps: Сформированная строка на вывод в окно
				char*  	msg=UNS_MALLOC(scrsize+20);								//sps: Сформированное сообщение для вывода на экран

				char*  	hxblok=UNS_MALLOC(hstrsz*2+1);							//sps: Блок шестнадцтеричных значений считанных байт
				char*  	asblok=UNS_MALLOC(hstrsz+1);							//sps: Блок ASCII значений считанных байт
				char*  	twohex=UNS_MALLOC(2);									//sps: Блок ASCII значений считанных байт

				if(offset==NULL || hexstr==NULL || msg==NULL || hxblok==NULL || asblok==NULL) return KEY_NONE;	//sps: Проверяем что все создалось правильно

				for (;;) {

					if(need_redraw)												//sps: если что-то поменялось - перерисовываем окно
					{

						memset(offset,0,tstrsz+1);								//sps: Чистим фсе
						memset(hexstr,0,tstrsz+1);
						memset(msg,0,scrsize+20);

						char*	pmsg=msg;										//sps: Создаем указатель для склеивания всех блоков в страницу

//-----------------------------------------------------------------------------------------------
						for(int j=point;j<point+hscrsze;j+=hstrsz)				//sps: формируем пять строк
						{

							memset(hxblok,0,hstrsz*2+1);						//sps: Чистим фсе
							memset(asblok,0,hstrsz+1);

							for(int i=j;i<j+hstrsz;i++)							//sps: формируем hxblok и asblok
							{
								sprintf(twohex,"%02X",wbuf[i]);					//sps: hxblok - шестнадцтеричная форма байт в ASCII
								hxblok[(i-j)*2]=twohex[0];
								hxblok[(i-j)*2+1]=twohex[1];

								if(wbuf[i]<' ')									//sps: asblok - ASCII имволы байт, с заменой спецсимволов
								{
									asblok[i-j]=' ';
								}else{
									asblok[i-j]=wbuf[i];
								}
							}

							sprintf(offset,"OFFSET:%08X",j);								//sps: получаем мещение OFFSET в шестнадцтеричном формате по строкам
							sprintf(hexstr,"%c %s %s",offset[14],hxblok,asblok);			//sps: Клеем симпатичную строчку, а она ломается)
							pmsg+=sprintf(pmsg,"%s",hexstr);								//sps: Клеем текстовый блок

						}
						sprintf(offset,"OFFSET:%08X",point);								//sps: получаем OFFSET первой строки в шестнадцтеричном формате
						asblok[scrsize+1]=0;												//sps: нуль-терменируем последнюю строку
//-----------------------------------------------------------------------------------------------
						MUTEX_LOCK(window->mutex)											//sps: зажали мютекс окна

							Screen_Clear(screen);
							Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_TXT);

							Screen_PutString(screen,offset,true);
							Screen_PutString(screen,msg,false);

						MUTEX_UNLOCK(window->mutex)											//sps: отдали мютекс окна
						need_redraw=false;													//sps: закрыли иф, пока кнопку не ткнут
					}
//-----------------------------------------------------------------------------------------------
				eKey key = LCDUI_Window_FetchKey(window);									//sps: проверяем кнопочки
						if (key != KEY_NONE)
						{
							if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
							{
								if(point<hstrsz){								//sps: Выравниваем начало файла
									point=hstrsz;
								}else{
									point-=hstrsz;
								}
								DBGF("point = %d %d",point,offs+point);
								if (offs+point<=offs && offs!=0)
								{
									point=hsize+hstrsz;
									offs-=hsize;
									ReGrab();
								};
							}
							else if ((key == KEY_DOWN || key == KEY_PGDOWN) && offs+point+hscrsze < size)
							{

								point+=hstrsz;
								DBGF("point = %d %d",point,offs+point);

								if (offs+point+hscrsze>=offs+wsize)
								{
									point=hsize-hscrsze;
									offs+=hsize;
									ReGrab();
								};
							}
							else if (key == KEY_RSOFT)
							{
								UNS_FREE(twohex);
								UNS_FREE(hexstr);
								UNS_FREE(msg);
								UNS_FREE(offset);
								UNS_FREE(hxblok);
								UNS_FREE(asblok);
								return key;
								break;
							}
							else if (key == KEY_LSOFT) {
								UNS_FREE(twohex);
								UNS_FREE(hexstr);
								UNS_FREE(msg);
								UNS_FREE(offset);
								UNS_FREE(hxblok);
								UNS_FREE(asblok);
								return key;
								break;
							}
							else if (key == KEY_PRINT)		//sps: [►☻◄ АДСКИЙ КОСТЫЛЬ ►☻◄]
							{
								msg[0]='\n';
								msg[15]='\n';
								msg[30]='\n';
								msg[45]='\n';
								msg[60]='\n';
								printMessage(msg);
							} else {
								beepError();
							}
							need_redraw=true;
							continue;
						}
//-----------------------------------------------------------------------------------------------
				}
				return KEY_NONE;
			}
//================================================================================================
//================================================================================================ SPS :: TxtViewer
			eKey TxtView(sLCDUI_Window* window) {

							sScreen* screen = &window->screen;

							bool need_redraw=true;											//sps: Пнуть в ТРУ если нужно перисовать окошко
							char*  msg=UNS_MALLOC(scrsize+20);								//sps: Сформированное сообщение для вывода на экран

							if(msg==NULL) return KEY_NONE;

//-----------------------------------------------------------------------------------------------
							for (;;) {
								if(need_redraw)												//sps: если что-то поменялось - перерисовываем окно
								{
//----------------------------------------------------------------------------------------------
									memset(msg,0,scrsize+20);								//sps: Чистим фсе
									int i;
									for(i=point;i<point+scrsize;i++)						//sps: "TXTBUF CONSTRUCTOR Lite" Формируем шесть строк на экране
									{
										if (i>=size) break;									//sps: Конец файла?  Ну так валим отсюда
										if(wbuf[i]<' ')
										{ msg[i-point]=' '; }
										else
										{ msg[i-point]=wbuf[i]; }
									}
									msg[i-point]=0;
//----------------------------------------------------------------------------------------------
									MUTEX_LOCK(window->mutex)								//sps: зажали мютекс окна

										Screen_Clear(screen);
										Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_HEX);

										Screen_PutString(screen,msg,false);					//sps: Отрисовка окна без курсором

									MUTEX_UNLOCK(window->mutex)								//sps: отдали мютекс окна
									need_redraw=false;										//sps: закрыли иф, пока кнопку не ткнут
								}
//-----------------------------------------------------------------------------------------------
							eKey key = LCDUI_Window_FetchKey(window);						//sps: проверяем кнопочки
									if (key != KEY_NONE)
									{
										if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
										{

											if(point<tstrsz){								//sps: Выравниваем начало файла
												point=tstrsz;
											}else{
												point-=tstrsz;
											}

											DBGF("point = %d %d",point,offs+point);

											if (offs+point<=offs && offs!=0)
											{
												point+=hsize;
												offs-=hsize;
												ReGrab();
											}
										}
										else if ((key == KEY_DOWN || key == KEY_PGDOWN) && offs+point+scrsize < size)
										{
											point+=tstrsz;
											DBGF("point = %d %d",point,offs+point);

											if (offs+point+scrsize>=offs+wsize)
											{
												point-=hsize;
												offs+=hsize;
												ReGrab();
											}
										}
										else if (key == KEY_RSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_LSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_PRINT)
										{
											printMessage(wbuf);
										} else {
											beepError();
										}
										need_redraw=true;
										continue;
									}
//-----------------------------------------------------------------------------------------------
							}
							return KEY_NONE;
						}
//================================================================================================
//================================================================================================ SPS :: TxtViewer
			eKey TxtEdit(sLCDUI_Window* window) {

							sScreen* screen = &window->screen;

							bool need_redraw=true;											//sps: Пнуть в ТРУ если нужно перисовать окошко
							char*  msg=UNS_MALLOC(scrsize+20);								//sps: Сформированное сообщение для вывода на экран

							if(msg==NULL) return KEY_NONE;

//-----------------------------------------------------------------------------------------------
							for (;;) {
								if(need_redraw)												//sps: если что-то поменялось - перерисовываем окно
								{
//----------------------------------------------------------------------------------------------
									memset(msg,0,scrsize+20);								//sps: Чистим фсе
									int i;
									for(i=point;i<point+scrsize;i++)						//sps: "TXTBUF CONSTRUCTOR Lite" Формируем шесть строк на экране
									{
										if (i>=size) break;									//sps: Конец файла?  Ну так валим отсюда
										if(wbuf[i]<' ')
										{ msg[i-point]=' '; }
										else
										{ msg[i-point]=wbuf[i]; }
									}
									msg[i-point]=0;
//----------------------------------------------------------------------------------------------
									MUTEX_LOCK(window->mutex)								//sps: зажали мютекс окна

										Screen_Clear(screen);
										Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_HEX);

										for(i=0;i<scrsize;i++)								//sps: Отрисовка окна с курсором
										{
											if(i==cursor){
												Screen_PutChar(screen,msg[i],true);
											}else{
												Screen_PutChar(screen,msg[i],false);
											}
										}

									MUTEX_UNLOCK(window->mutex)								//sps: отдали мютекс окна
									need_redraw=false;										//sps: закрыли иф, пока кнопку не ткнут
								}
//-----------------------------------------------------------------------------------------------
							eKey key = LCDUI_Window_FetchKey(window);						//sps: проверяем кнопочки
									if (key != KEY_NONE)
									{
										if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
										{
										cursor-=tstrsz;
										if(cursor<0)
										{
											cursor+=tstrsz;

											if(point<tstrsz){								//sps: Выравниваем начало файла
												point=tstrsz;
											}else{
												point-=tstrsz;
											}

											DBGF("point = %d %d",point,offs+point);

											if (offs+point<=offs && offs!=0)
											{
												point+=hsize;
												offs-=hsize;
												ReGrab();
											};
										}
										}
										else if ((key == KEY_DOWN || key == KEY_PGDOWN) && offs+point+scrsize < size)
										{
											cursor+=tstrsz;
											if(cursor>scrsize)
											{
												cursor-=tstrsz;
												point+=tstrsz;
												DBGF("point = %d %d",point,offs+point);

												if (offs+point+scrsize>=offs+wsize)
												{
												point-=hsize;
													offs+=hsize;
													ReGrab();
												}
											}
										}
										else if ((key == KEY_LEFT) && cursor > 0)
										{
											cursor--;
										}
										else if ((key == KEY_RIGHT) && cursor < scrsize)
										{
											cursor++;
										}
										else if (key == KEY_RSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_LSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_PRINT)
										{
											printMessage(wbuf);
										} else {
											beepError();
										}
										need_redraw=true;
										continue;
									}
//-----------------------------------------------------------------------------------------------
							}
							return KEY_NONE;
						}
//================================================================================================
//-----------------------------------------------------------------------------------------------
			sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();

			ReGrab();	//sps: Захватываем первую часть файла в скользящий буфер

			for(;;)
			{
				eKey rkey = TxtView(window);								//sps: Открываем TXT-просмотрщик
				if(rkey==KEY_RSOFT)											//sps: Смена вида?
				{

					point=(point/hstrsz)*hstrsz;							//sps: Уравнитель POINT-a "TXT>>HEX" (Выравниваем точку просмотра по началу строки)

					rkey = HexView(window);									//sps: Открываем HEX-просмотрщик
					if(rkey==KEY_LSOFT){break;}								//sps: Закрыть просмотр

					point=(point/tstrsz)*tstrsz;							//sps: Уравнитель POINT-a "HEX>>TXT" (Выравниваем точку просмотра по началу строки)
					}
					else{break;}											//sps: Закрыть просмотр
			}
//-----------------------------------------------------------------------------------------------
		UNS_FREE(wbuf);
		return true;
}

/*
static bool readFileEx(FILINFO* fno, char* full_path){
	#ifndef FUNCTION_DEBUG
	if(fno->fattrib & AM_WRO)
	{
		beepError();
		toast_access_denied();
		return false;
	}
	#endif
//-----------------------------------------------------------------------------------------------
	char* 	bitbuf=UNS_MALLOC(fno->fsize+1);

	int    	point=0;								//sps: Позиция на которой сейчас отображаемый текст
	int    	size=fno->fsize;						//sps: Размер открываемого файла

//-----------------------------------------------------------------------------------------------

	FIL fil;
	FRESULT fres=f_open(&fil,full_path,FA_READ);
	if(fres==FR_OK)
	{
		unsigned int len;
		fres=f_read(&fil,bitbuf,fno->fsize,&len);

//================================================================================================ SPS :: HexViewer
			eKey HexView(sLCDUI_Window* window) {

				sScreen* screen = &window->screen;

				bool need_redraw=true;											//sps: Пнуть в ТРУ если нужно перисовать окошко
				char*  offset=UNS_MALLOC(15+1);									//sps: Смещение номера считанного байта файла в НЕХ
				char*  hexstr=UNS_MALLOC(15+1);									//sps: Сформированная строка на вывод в окно
				char*  msg=UNS_MALLOC(80+20);										//sps: Сформированное сообщение для вывода на экран

				char*  hxblok=UNS_MALLOC(8+1);									//sps: Блок шестнадцтеричных значений считанных байт
				char*  asblok=UNS_MALLOC(4+1);									//sps: Блок ASCII значений считанных байт

				if(offset==NULL || hexstr==NULL || msg==NULL || hxblok==NULL || asblok==NULL) return KEY_NONE;

				for (;;) {


					if(need_redraw)												//sps: если что-то поменялось - перерисовываем окно
					{
						sprintf(offset,"");										//sps: Чистим фсе
						sprintf(hexstr,"");
						sprintf(msg,"");

//-----------------------------------------------------------------------------------------------
						for(int j=point;j<point+20;j+=4)									//sps: формируем пять строк
						{
							sprintf(hxblok,"");												//sps: Чистим фсе
							sprintf(asblok,"");

							for(int i=j;i<j+4;i++)											//sps: формируем hxblok и asblok
							{
								sprintf(hxblok,"%s%02X",hxblok,bitbuf[i]);						//sps: hxblok - шестнадцтеричная форма байт в ASCII

								if(bitbuf[i]<' ')												//sps: asblok - ASCII имволы байт, с заменой спецсимволов
								{
									sprintf(asblok,"%s ",asblok);
								}else{
									sprintf(asblok,"%s%c",asblok,bitbuf[i]);
								}
							}
							sprintf(offset,"OFFSET:%08X",j);								//sps: получаем мещение OFFSET в шестнадцтеричном формате по строкам
							sprintf(hexstr,"%c %s %s",offset[14],hxblok,asblok);			//sps: Клеем симпатичную строчку, а она ломается)
							sprintf(msg,"%s%s",msg,hexstr);									//sps: Клеем текстовый блок
						}
						sprintf(offset,"OFFSET:%08X",point);								//sps: получаем OFFSET первой строки в шестнадцтеричном формате
//-----------------------------------------------------------------------------------------------
						MUTEX_LOCK(window->mutex)								//sps: зажали мютекс окна

						Screen_Clear(screen);
						Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_TXT);

						Screen_PutString(screen,offset,true);
						Screen_PutString(screen,msg,false);

						MUTEX_UNLOCK(window->mutex)								//sps: отдали мютекс окна
						need_redraw=false;										//sps: закрыли иф, пока кнопку не ткнут
					}
//-----------------------------------------------------------------------------------------------
				eKey key = LCDUI_Window_FetchKey(window);						//sps: проверяем кнопочки
						if (key != KEY_NONE)
						{
							if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
							{point-=4;}
							else if ((key == KEY_DOWN || key == KEY_PGDOWN) && point+20 < size)
							{point+=4;}
							else if (key == KEY_RSOFT)
							{
								UNS_FREE(hexstr);
								UNS_FREE(msg);
								UNS_FREE(offset);
								UNS_FREE(hxblok);
								UNS_FREE(asblok);
								return key;
								break;
							}
							else if (key == KEY_LSOFT) {
								UNS_FREE(hexstr);
								UNS_FREE(msg);
								UNS_FREE(offset);
								UNS_FREE(hxblok);
								UNS_FREE(asblok);
								return key;
								break;
							}
							else if (key == KEY_PRINT)
							{
								msg[0]='\n';
								msg[15]='\n';
								msg[30]='\n';
								msg[45]='\n';
								msg[60]='\n';
								printMessage(msg);
							} else {
								beepError();
							}
							need_redraw=true;
							continue;
						}
//-----------------------------------------------------------------------------------------------
				taskYIELD();													//sps: Разрешение выполняться для других задач
				}
				return KEY_NONE;
			}
//================================================================================================
//================================================================================================ SPS :: TxtViewer
			eKey TxtView(sLCDUI_Window* window) {

							sScreen* screen = &window->screen;

							bool need_redraw=true;											//sps: Пнуть в ТРУ если нужно перисовать окошко
							char*  msg=UNS_MALLOC(80+20);										//sps: Сформированное сообщение для вывода на экран

							if(msg==NULL) return KEY_NONE;
//-----------------------------------------------------------------------------------------------
							for (;;) {
								if(need_redraw)												//sps: если что-то поменялось - перерисовываем окно
								{
//----------------------------------------------------------------------------------------------
									sprintf(msg,"");										//sps: Чистим фсе
									for(int i=point;i<point+89;i++)							//sps: "TXTBUF CONSTRUCTOR Lite" Формируем шесть строк на экране
									{
										if (i>=size) break;									//sps: Конец файла?  Ну так валим отсюда
										if(bitbuf[i]<' ')
										{sprintf(msg,"%s ",msg);}
										else
										{sprintf(msg,"%s%c",msg,bitbuf[i]);}
									}
									sprintf(msg,"%s\0",msg);
//----------------------------------------------------------------------------------------------
									MUTEX_LOCK(window->mutex)								//sps: зажали мютекс окна

									Screen_Clear(screen);
									Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_HEX);

									Screen_PutString(screen,msg,false);

									MUTEX_UNLOCK(window->mutex)								//sps: отдали мютекс окна
									need_redraw=false;										//sps: закрыли иф, пока кнопку не ткнут
								}
//-----------------------------------------------------------------------------------------------
							eKey key = LCDUI_Window_FetchKey(window);						//sps: проверяем кнопочки
									if (key != KEY_NONE)
									{
										if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
										{point-=15;}
										else if ((key == KEY_DOWN || key == KEY_PGDOWN) && point+89 < size)
										{point+=15;}
										else if (key == KEY_RSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_LSOFT)
										{
											UNS_FREE(msg);
											return key;
											break;
										}
										else if (key == KEY_PRINT)
										{
											printMessage(bitbuf);
										} else {
											beepError();
										}
										need_redraw=true;
										continue;
									}
			//-----------------------------------------------------------------------------------------------
							taskYIELD();													//sps: Разрешение выполняться для других задач
							}
							return KEY_NONE;
						}
//================================================================================================
			if(fres==FR_OK)														//sps: Просмотрщик файла
			{
				sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();
				for(;;)
				{
					eKey rkey = TxtView(window);								//sps: Открываем TXT-просмотрщик
					if(rkey==KEY_RSOFT)											//sps: Смена вида?
					{
						point=(point/4)*4;												//sps: Великий уравнитель POINT-a "TXT>>HEX" (Выравниваем точку просмотра по началу строки)

						rkey = HexView(window);									//sps: Открываем HEX-просмотрщик
						if(rkey==KEY_LSOFT){break;}								//sps: Закрыть просмотр

						point=(point/15)*15;									//sps: Великий уравнитель POINT-a "HEX>>TXT" (Выравниваем точку просмотра по началу строки)
					}
					else{break;}												//sps: Закрыть просмотр
				}
			}
//-----------------------------------------------------------------------------------------------
		f_close(&fil);
		UNS_FREE(bitbuf);
	}
	return true;
}
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*//sps: [►☻◄ АДСКИЙ КОСТЫЛЬ ►☻◄]
static bool readFile(FILINFO* fno, char* full_path){
	#ifndef FUNCTION_DEBUG
	if(fno->fattrib & AM_WRO)
	{
		beepError();
		toast_access_denied();
		return false;
	}
	#endif

	char* buf;

	DBGF("Size of FILE = %d",fno->fsize);

	if(fno->fsize > 32768  || (buf=UNS_MALLOC(fno->fsize+1))==NULL)
	{
		DBG("File too large!");
		LCD_ReadmeWithBackEmptyButtons(LANG_ERROR_MEMORY,JUSTIFY_CENTER);
		return false;
	}

	FIL fil;
	FRESULT fres=f_open(&fil,full_path,FA_READ);
	if(fres==FR_OK)
	{
		unsigned int len;
		fres=f_read(&fil,buf,fno->fsize,&len);

		if(fres==FR_OK)
		{
			for(int i=0;i<fno->fsize;i++)			//killing spec.chars
			{
				if(buf[i]<' ' && buf[i]!='\n') buf[i]=' ';
			}
			buf[fno->fsize]='\0';
			LCD_ReadmeWithBackEmptyButtons(buf,JUSTIFY_NONE);
		}
		f_close(&fil);
	}
	UNS_FREE(buf);
	return true;
}
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void diskExplorer(){
	#ifdef FUNCTION_uSD
	restart:;
	#endif
	char* initial_path;
	FATFS* fs=NULL;
	#ifdef FUNCTION_uSD
		signed int chosenDisk=0;
		sDynMenu* menu = DynMenuCreateWithCapacity("\n"LANG_DISKEXP_CHOOSE_DISK":", 2);
		if(menu==NULL) return;
		DynMenuAddItem(menu,LANG_DISKEXP_INTERNAL_MEM);
		DynMenuAddItem(menu,LANG_DISKEXP_SD_CARD);
		DynMenuSetCursor(menu,chosenDisk);
		chosenDisk=DynMenuShow(menu);
		DynMenuDelete(menu);
		if(chosenDisk==-1) return;
		initial_path=(chosenDisk==0)?"0:":"1:";
		fs=(chosenDisk==0)?&FS_DF:&FS_SD;
	#else
		initial_path="0:";
		fs=&FS_DF;
	#endif

	char temp[LCD_CLIENT_WIDTH+1];

	char* megapath=UNS_MALLOC(1024);
	if(megapath==NULL) return;
	strcpy(megapath,initial_path);

	char* megapath_readable=NULL;

	while(1){
		signed int chosen;

		int num_files;
		int num_folders;
		FRESULT res=f_getfilescount(megapath, &num_files, &num_folders);
		if(res!=FR_OK){
			toast_disk_read_error();
			break;
		}
		int num_total=num_files+num_folders;

		if(megapath_readable)	UNS_FREE(megapath_readable);
		megapath_readable=UNS_MALLOC_STRING(megapath);
		convert_866to1251_inbuf(megapath_readable);

		char* caption=megapath_readable;
		if(strlen(megapath)==2){
			int percent=100-(DRV_get_free_space(fs)*100/DRV_get_total_space(fs));
			memset(temp,' ',sizeof(temp));
			memcpy(temp,megapath,2);
			temp[2]='/';
			sprintf(temp+LCD_CLIENT_WIDTH-3,"%2d%%",percent);
			caption=temp;
		}

		if(num_total==0){
			char* tmp=UNS_MALLOC_STRING(caption);
			if(tmp){
				LCD_ReadmeWithBackEmptyButtons(tmp,JUSTIFY_NONE);
				UNS_FREE(tmp);
			}
			chosen=-1;
		}else{

			sDynMenu* menu = DynMenuCreateWithCapacity(caption, num_total);
			if(menu==NULL) break;

			eDynMenuKeyHandlerResult KeyHandler(eKey key, int index){
				if(key==KEY_UP || key==KEY_DOWN || key==KEY_LEFT || key==KEY_RIGHT || key==KEY_LSOFT) return DYNMENU_KEYHANDLER_NOT_HANDLED;

				eDynMenuKeyHandlerResult retval=DYNMENU_KEYHANDLER_HANDLED;

				char* new_path=UNS_MALLOC(strlen(megapath)+13);


				if(new_path){
					char short_readable_name[LCD_CLIENT_WIDTH+1];
					strcpy(short_readable_name,DynMenuGetItemAtIndex(menu,index)->item_name);
					char* ptr_space=strchr(short_readable_name,' ');
					if(ptr_space){*ptr_space='\0';}
					char* short_name=temp;
					strcpy(short_name,short_readable_name);
					convert_1251to866_inbuf(short_name);

					sprintf(new_path,"%s/%s",megapath,short_name);

					FILINFO fno;
					FRESULT fres=f_stat(new_path,&fno);
					bool is_folder=fno.fattrib&AM_DIR;


					if(fres!=FR_OK)
					{
						DBG("fres!=FR_OK");
						toast_file_not_found(short_readable_name);
					}
//--------------------------------------------------------------------------------------------------------------------------
//	Обработка кнопок работы с файлами. Дополнено :: SPS :: 2015
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_1)									//sps: Справка
					{
						LCDUI_Supervisor_Toast(LANG_DISKEXP_HELP,2000);
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_8)									// Удалить файл
					{
						if(deleteFile(&fno,new_path,short_readable_name)){
							need_rebuild=true;
							retval=DYNMENU_KEYHANDLER_SHOULD_CANCEL;
						}
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_3)									// Чтение содержимого файла
					{
						readFileBf(&fno,new_path);
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_4)									//sps: Переименовать / переместить файл
					{
						renameFile(&fno,megapath,new_path);				//sps: Функция переименовывания файла

						need_rebuild=true;								// Обновляем окно после переименовывания
						retval=DYNMENU_KEYHANDLER_SHOULD_CANCEL;		// Чтобы убрать зависшее в памяти окна старое имя файла
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_6)									//sps: Дублировать файл
					{
						duplicFile(&fno,megapath,short_name);			//sps: Функция дублирования файла


						need_rebuild=true;								// Обновляем окно после переименовывания
						retval=DYNMENU_KEYHANDLER_SHOULD_CANCEL;		// Чтобы убрать зависшее в памяти окна старое имя файла
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_OPL)								// Старт файла
					{
						runFile(new_path,short_readable_name);
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key==KEY_RSOFT)								// Открытие папки, если это папка
					{
						if(is_folder){
							strcpy(megapath,new_path);
							need_rebuild=true;
							retval=DYNMENU_KEYHANDLER_SHOULD_CANCEL;
						}else{
							showFileInfo(&fno,short_readable_name);
						}
					}
//--------------------------------------------------------------------------------------------------------------------------
					else if(key>=KEY_0 && key<=KEY_9)					//sps: Если нажато чет левое - пикним с негодованием!
					{
						beepError();
					}
//--------------------------------------------------------------------------------------------------------------------------
					else
					{
						retval=DYNMENU_KEYHANDLER_NOT_HANDLED;
					}

					UNS_FREE(new_path);
				}
				return retval;
			}
			DynMenuSetKeyHandler(menu,KeyHandler);
			DynMenuSetCaptionInverted(menu,true);

			char temp[LCD_CLIENT_WIDTH+1];

			bool iterator(char* fn, FILINFO* fno, bool is_folder){
				convert_866to1251_inbuf(fn);
				if(is_folder) {
					to_uppercase(fn);
					memset(temp,' ',LCD_CLIENT_WIDTH);
					memcpy(temp,fn,strlen(fn));
					temp[LCD_CLIENT_WIDTH-1]=LCD_SYMBOL_TRIANGLE_RIGHT;
					temp[LCD_CLIENT_WIDTH]='\0';
				}else{
					strcpy(temp,fn);
				}
				DynMenuAddItem(menu,UNS_MALLOC_STRING(temp));
				return false;
			}
			f_iterate_folder(megapath,iterator);

			chosen=DynMenuShow(menu);
			DynMenuDeleteWithCleaner(menu,SimpleFreeCleaner);
		}

		if(chosen==-1) {
			if(need_rebuild) {
				need_rebuild=false;
			}else{
				char* last_slash=strrchr(megapath,'/');
				if(last_slash==NULL) break;
				*last_slash='\0';
			}
		}
	}//of while(1)

	if(megapath_readable!=NULL)	UNS_FREE(megapath_readable);
	UNS_FREE(megapath);

	#ifdef FUNCTION_uSD
	goto restart;
	#endif
}

void ActionDiskExplorer(void){
	//createWindowTask("Disk Explorer",TASK_STACKSIZE_DEFAULT,diskExplorer);
	diskExplorer();
}
