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
#include "dex_read_file.h"		//sps


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
		if(fno->fattrib & AM_DIR)		//sps: Запрещаем открытие папок через просмотрщик / редактор
			{
				beepError();
				return false;
			}
		//if ((fno->fattrib & AM_LFN) == AM_LFN) TODO Это пример, если не нужен - удалить
//-----------------------------------------------------------------------------------------------

		int    			point=0;				//sps: Позиция на которой сейчас отображаемый текст
		int    			size=fno->fsize;		//sps: Размер открываемого файла

//-----------------------------------------------------------------------------------------------

		#define hsize 512															//sps:	Размер половины буфера
		#define wsize hsize*2														//sps:	Размер буфера

		#define hstrsz 	4															//sps:	Кол-во обрабатываемых символов в строке HEX
		#define scrsize LCD_CLIENT_WIDTH*LCD_CLIENT_HEIGHT							//sps:	Кол-во символов на экране в TXT
		#define hscrsze hstrsz*(LCD_CLIENT_HEIGHT-1)								//sps:	Кол-во символов на экране в HEX
		#define tstrsz	LCD_CLIENT_WIDTH											//sps:	Кол-во обрабатываемых символов в строке TXT
		#define slnum 	1															//sps:	Кол-во имволов для отображения смещения адреса

		#define spcount2 (LCD_CLIENT_WIDTH-(hstrsz*2+hstrsz+slnum))/2				//sps: Вычисляем ширину пробелов в зависимости от ширины экрана
		#define spcount1 (LCD_CLIENT_WIDTH-(hstrsz*2+hstrsz+slnum))-spcount2
		#define hex_Lmarg (spcount1+slnum)											//sps: Ширина левого отступа в НЕХ-просмотре
		#define hex_Rmarg (spcount2)												//sps: Ширина правого отступа в НЕХ-просмотре

//-----------------------------------------------------------------------------------------------

		unsigned int len;															//sps:	Возврат прочитанных байт
		unsigned int offs=0;														//sps:	Смещение буфера по файлу
		unsigned int grab;															//sps:	Кол-во загружаеммых в буфер байт

		char* 		 wbuf=UNS_MALLOC(wsize+1);										//sps:	Буфер

		bool		 chestat=false;													//sps: 	Было ли редактирование?
		bool 		 need_redraw;													//sps: 	Пнуть в ТРУ если нужно перисовать окошко при перемещении курсора
		bool 		 need_reconstruct;												//sps: 	Пнуть в ТРУ если нужно перисовать окошко новой инфой

//-----------------------------------------------------------------------------------------------

		char*  		offset=UNS_MALLOC(tstrsz);										//sps: Смещение номера считанного байта файла в НЕХ
		char*  		hexstr=UNS_MALLOC(tstrsz);										//sps: Сформированная строка на вывод в окно
		char*  		msg=UNS_MALLOC(scrsize);										//sps: Сформированное сообщение для вывода на экран

		char*  		hxblok=UNS_MALLOC(hstrsz*2+1);									//sps: Блок шестнадцтеричных значений считанных байт
		char*  		asblok=UNS_MALLOC(hstrsz+1);									//sps: Блок ASCII значений считанных байт
		char 		twohex[2];														//sps: Бфер для двухсимольного значения байта в НЕХ-е

//-----------------------------------------------------------------------------------------------

		char 		space1[spcount1+1];
		char 		space2[spcount2+1];

		for(int i=0;i<spcount1;i++)													//sps: Набиваем пробелы для первого и второго столбца
				{
					if(spcount1-i > 0)space1[i] = ' ';
					if(spcount2-i > 0)space2[i] = ' ';
				}
		space1[spcount1]=0;
		space2[spcount2]=0;

//-----------------------------------------------------------------------------------------------

		//sps: Проверяем что все создалось правильно
		if(offset==NULL || hexstr==NULL || msg==NULL || hxblok==NULL || asblok==NULL/* || space1==NULL */|| space2==NULL) return false;

//================================================================================================
// SPS :: Функция загрузки и обработки скользящего буфера
//================================================================================================
			void ReGrab(bool rewrite)											//sps: Захват части файла в буфер
			{
					FIL 	fil;

//----------------------------------------------------------------- HALF-BUFF

		 	 	 	if(rewrite==1) {

						FRESULT fres=f_open(&fil,full_path, FA_WRITE);			//sps:	Открываем файл на запись

						if(fres==FR_OK)
						{
							if(((fno->fsize)-offs)>=wsize){grab=wsize;}else{grab=((fno->fsize)-offs);}		//sps:	Вычисление кол-ва байт

							f_lseek(&fil, offs);								//sps:	Сдвигаем позицию считывания
							f_write(&fil, wbuf, grab, &len);					//sps:	Пишем все что изменили
							chestat=false;										//sps:  Изменения в буфере сохранены
						}
						f_close(&fil);											//sps:	Закрываем файл
					}

					FRESULT fres=f_open(&fil,full_path,FA_READ);				//sps:	Открываем файл на чтение

		  			if(fres==FR_OK)
					{
						memcpy(wbuf,wbuf+hsize,hsize);							//sps:	Переносим нижние пол буфера вверх

							if(offs!=0){
								grab=hsize;
								f_lseek(&fil, offs+hsize);						//sps:	Сдвигаем позицию считывания
							}else{
								grab=wsize;
								f_lseek(&fil, offs);							//sps:	Сдвигаем позицию считывания
							}

						memset(wbuf+hsize,0,hsize);								//sps: Чистим вторые пол буфера т.к. заполним его не полностью

						if(offs!=0){
							fres=f_read(&fil,wbuf+hsize,grab,&len);				//sps:	Читаем из файла пол буфера
						}else{
							fres=f_read(&fil,wbuf,grab,&len);					//sps:	Читаем из файла целый буфер
						}

						wbuf[offs+grab]=0;										//sps:	Затыкаем строку в буфере
						DBGF("offs+grab = %d",offs+grab)
					}

						f_close(&fil);											//sps:	Закрываем файл
			}
//================================================================================================
// SPS :: Функция замены символов для Hex-редактора
//================================================================================================
			void ChangeCHAR(char* msg, int mcx, int mcy, int scx, int scy, char key)
			{
				char 	hidhex[2];			 									//sps: Буфер для обратного конвертирования НЕХ представления символа из двух символов в один символ ASCII
				int		icursor;			 									//sps: Вычесляем позицию курсора в буфере для замены симола
				int		hcursor;												//sps: Вычесляем позицию курсора в просмотре хекса для замены симола
				int 	indexch;												//sps: Индекс символа в массиве
				char 	hexchar[6] = {'A','B','C','D','E','F'};					//sps: Набор символов для воода в HEX

				icursor = (point+mcy*hstrsz+mcx/2)-1;							//sps: Вычесляем позицию курсора в буфере для замены симола
				hcursor = (LCD_CLIENT_WIDTH*mcy+mcx)-1;

				for(indexch=0;indexch<=5;indexch++){							//sps: Вычесляем индекс буквы или цифру на текущей позиции курсора
					if (hexchar[indexch]==msg[hcursor+1])break;
				}

				if(key=='X')													//sps: Если запрос на ввод буквы
				{
					if(indexch<=5){												//sps: И в текущей позиции была буква
						if(indexch==5){											//sps: Циклически подставляем следующую букву
							msg[hcursor+1]=hexchar[0];
						}else{
							msg[hcursor+1]=hexchar[indexch+1];
						}
					}else{														//sps: Или поставляем первую "А" если там была цифра
						msg[hcursor+1]=hexchar[0];
					}
				}else{
					msg[hcursor+1]=key;											//sps: Заменяем символ цифрой
				}

				if(((mcy*hstrsz*2+mcx)-1)&1){									//sps: Определяем на какой части хекс-представления символа находится курсор
					sprintf(hidhex,"%c%c",msg[hcursor+1],msg[hcursor+2]);		//sps: Забираем два нужных символа при нечетином курсоре
				}else{
					sprintf(hidhex,"%c%c",msg[hcursor],msg[hcursor+1]);			//sps: Забираем два нужных символа при четином курсоре
				}

				wbuf[icursor] = strtol(hidhex, NULL, 16);						//sps: Записываем новый символ в буфер

				need_reconstruct=true;											//sps: Перестраеваем окно
				chestat=true;													//sps: Буфер редактировался
			}
//================================================================================================
// SPS :: Функция перелистывания НЕХ-страниц вверх
//================================================================================================
			void HexScreenUp()
			{
				if (point > 0)													//sps: Контроллируем верхний край файла
				{
					if(point<hstrsz){											//sps: Выравниваем начало файла
						point=hstrsz;
					}else{
						point-=hstrsz;
					}

					if (offs+point<=offs && offs!=0)							//sps: Буфер закончился, двигаем его вверх, если есть куда
					{
						point=hsize+hstrsz;
						offs-=hsize;
						if(chestat){											//sps: Если были изменения в буфере, предложим сохранить
							eKey res=LCD_ReadmeWithNoYesButtons(LANG_HEXEDIT_WRASK,JUSTIFY_CENTER);
							if(res==KEY_RSOFT){ReGrab(true);}else{ReGrab(false);}
						}else{ReGrab(false);}
					}
				}
			}
//================================================================================================
// SPS :: Функция перелистывания НЕХ-страниц вниз
//================================================================================================
			void HexScreenDown()
			{
					point+=hstrsz;												//sps: Двигаем экран

					if (offs+point+hscrsze>=offs+wsize)							//sps: Если кончился буфер, загружаем новый кусок
					{
						point=hsize-hscrsze;
						offs+=hsize;
						if(chestat){											//sps: Если были изменения в буфере, предложим сохранить
							eKey res=LCD_ReadmeWithNoYesButtons(LANG_HEXEDIT_WRASK,JUSTIFY_CENTER);
							if(res==KEY_RSOFT){ReGrab(true);}else{ReGrab(false);}
						}else{ReGrab(false);}
					}
			}
//================================================================================================
// SPS :: Перестройка окна НЕХ-просмотрщика/редактора
//================================================================================================
			void HexReconstruct(bool offslide)										//sps: если что-то поменялось - перерисовываем окно
			{
				memset(offset,0,tstrsz+1);											//sps: Чистим фсе
				memset(hexstr,0,tstrsz+1);
				memset(msg,0,scrsize);

				char*	pmsg=msg;													//sps: Создаем указатель для склеивания всех блоков в страницу

//-----------------------------------------------------------------------------------------------
				for(int j=point;j<point+hscrsze;j+=hstrsz)							//sps: формируем пять строк
				{

					memset(hxblok,0,hstrsz*2);									//sps: Чистим фсе
					memset(asblok,0,hstrsz);

					for(int i=j;i<j+hstrsz;i++)										//sps: формируем hxblok и asblok
					{
						sprintf(twohex,"%02X",wbuf[i]);								//sps: hxblok - шестнадцтеричная форма байт в ASCII
						hxblok[(i-j)*2]=twohex[0];
						hxblok[(i-j)*2+1]=twohex[1];

						if ((offs+i)>((fno->fsize)-1)){								//sps: Забиваем пробелами все что больше размера файла
							hxblok[(i-j)*2]=LCD_SYMBOL_CHESS;
							hxblok[(i-j)*2+1]=LCD_SYMBOL_CHESS;
						}

						if(wbuf[i]<' ')												//sps: asblok - ASCII имволы байт, с заменой спецсимволов
						{
							asblok[i-j]=' ';
						}else{
							asblok[i-j]=wbuf[i];
						}
					}

					if (offslide){
						sprintf(offset,"%08X",offs+j);						//sps: получаем смещение OFFSET в шестнадцтеричном формате по строкам (скользящее)
					}else{
						sprintf(offset,"%08X",j-point);						//sps: получаем смещение OFFSET в шестнадцтеричном формате по строкам (неподвижное)
					}

					//sps: Клеем симпатичную строчку, а она ломается)
					sprintf(hexstr,"%c%s%s%s%s",offset[7],space1,hxblok,space2,asblok);

					pmsg+=sprintf(pmsg,"%s",hexstr);								//sps: Клеем текстовый блок
				}
			}
//================================================================================================
// SPS :: Печать окна НЕХ-просмотрщика/редактора
//================================================================================================
			void printHex()
			{
				char*  	hpmsg=UNS_MALLOC(scrsize+20);

				memset(offset,0,tstrsz+1);											//sps: Чистим фсе
				memset(hexstr,0,tstrsz+1);
				memset(hpmsg,0,scrsize+20);

				char*	pmsg=hpmsg;													//sps: Создаем указатель для склеивания всех блоков в страницу

				pmsg+=sprintf(pmsg,"OFFSET:%08X",offs+point);						//sps: получаем OFFSET первой строки в шестнадцтеричном формате
				pmsg+=sprintf(pmsg,"\n");											//sps: Форматируем строку перемещением каретки Форматируем строку

//-----------------------------------------------------------------------------------------------
				for(int j=point;j<point+hscrsze;j+=hstrsz)							//sps: формируем пять строк
				{

					memset(hxblok,0,hstrsz*2);									//sps: Чистим фсе
					memset(asblok,0,hstrsz);

					for(int i=j;i<j+hstrsz;i++)										//sps: формируем hxblok и asblok
					{
						sprintf(twohex,"%02X",wbuf[i]);								//sps: hxblok - шестнадцтеричная форма байт в ASCII
						hxblok[(i-j)*2]=twohex[0];
						hxblok[(i-j)*2+1]=twohex[1];

						if ((offs+i)>((fno->fsize)-1)){								//sps: Забиваем пробелами все что больше размера файла
							hxblok[(i-j)*2]=LCD_SYMBOL_FREESPACE;
							hxblok[(i-j)*2+1]=LCD_SYMBOL_FREESPACE;
						}

						if(wbuf[i]<' ')												//sps: asblok - ASCII имволы байт, с заменой спецсимволов
						{
							asblok[i-j]=' ';
						}else{
							asblok[i-j]=wbuf[i];
						}
					}

					sprintf(offset,"OFFSET:%08X",offs+j);							//sps: получаем смещение OFFSET для выборки последнего символа
					sprintf(hexstr,"%c%s%s%s%s",offset[14],space1,hxblok,space2,asblok);
					pmsg+=sprintf(pmsg,"%s",hexstr);								//sps: Клеем текстовый блок
					pmsg+=sprintf(pmsg,"\n");										//sps: Форматируем строку перемещением каретки
				}
//-----------------------------------------------------------------------------------------------
				printMessage(hpmsg);
				UNS_FREE(hpmsg);
				HexReconstruct(false);												//sps: Обновляем окно TODO уйти от общих для нескольких подфункций переменных
			}
//================================================================================================
// SPS :: HЕХ-просмотрщик
//================================================================================================
			eKey HexView(sLCDUI_Window* window) {

			need_redraw=true;														//sps: Пнуть в ТРУ если нужно перисовать окошко при редактировании
			need_reconstruct=true;													//sps: Пнуть в ТРУ если нужно перисовать окошко новой инфой

//-----------------------------------------------------------------------------------------------

			for (;;)
			{
				if(need_reconstruct){
					HexReconstruct(false);											//sps: Конструируем окно
					need_reconstruct=false;
				}

				//-----------------------------------------------------------------------------------------------

				if(need_redraw)														//sps: если что-то поменялось - перерисовываем окно
				{
					sScreen* screen = &window->screen;

					/////////////////////////////////////////////// ФОРМИРУЕМ ВЕРХНЮЮ СТРОКУ СМЕЩЕНИЯ ///////////////////////////////////////////////////

					sprintf(offset,"OFFSET:%08X",offs+point);						//sps: получаем OFFSET первой строки в шестнадцтеричном формате

					/////////////////////////////////////////////// БЛОК ОТРИСОВКИ СТРАНИЦЫ /////////////////////////////////////////////////////////////

					MUTEX_LOCK(window->mutex)										//sps: зажали мютекс окна

						Screen_Clear(screen);
						Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_OPTIONS);

						Screen_PutString(screen,offset,true);
						Screen_PutString(screen,msg,false);

					MUTEX_UNLOCK(window->mutex)										//sps: отдали мютекс окна

						need_redraw=false;											//sps: закрыли иф, пока кнопку не ткнут
				}

//-----------------------------------------------------------------------------------------------
				eKey key = LCDUI_Window_FetchKey(window);							//sps: проверяем кнопочки
				if (key != KEY_NONE)
				{
					if ((key == KEY_UP || key==KEY_PGUP) && point > 0)
					{
						HexScreenUp();												//sps: Листаем вверх
						HexReconstruct(false);										//sps: Конструируем окно
					}
						else if ((key == KEY_DOWN || key == KEY_PGDOWN) && offs+point+hscrsze < size)
					{
						HexScreenDown();											//sps: Листаем вниз
						HexReconstruct(false);										//sps: Конструируем окно
					}
						else if (key == KEY_RSOFT)									//sps: Уходим отсюда
					{
						return key;
						break;
					}
						else if (key == KEY_LSOFT) {
							return key;
							break;
						}
					else if (key == KEY_PRINT)
					{
						printHex();													//sps: Распечатать окно Hex-просмотрщика
					} else {
						beepError();
					}
						need_redraw=true;
						continue;
				}

				taskYIELD();
			}
//-----------------------------------------------------------------------------------------------
			return KEY_NONE;
		}
//================================================================================================
//  SPS ::  НЕХ-редактор
//================================================================================================
			eKey HexEdit(sLCDUI_Window* window) {

						need_redraw=true;													//sps: Пнуть в ТРУ если нужно перисовать окошко при редактировании
						need_reconstruct=true;												//sps: Пнуть в ТРУ если нужно перисовать окошко новой инфой

//-----------------------------------------------------------------------------------------------
				int    	cursor=0;															//sps: Позиция на которой сейчас основной курсор
				int    	slcurs=0;															//sps: Позиция на которой сейчас вторичный курсор
				int		mcx=hex_Lmarg, mcy=0;												//sps: Координаты основного указателя
				int		shad_cx=mcx, shad_cy=mcy;											//sps: Теневые координаты для предпроверки граничных условий
				int		scx=0, scy=0;														//sps: Координаты вторичного указателя
				UINT	fpoint;																//sps: Положение HEX-курсора в файле (для редактирования)

//-----------------------------------------------------------------------------------------------

				//sps: Проверяем что все создалось правильно
				if(offset==NULL || hexstr==NULL || msg==NULL || hxblok==NULL || asblok==NULL || space1==NULL || space2==NULL) return KEY_NONE;

//-----------------------------------------------------------------------------------------------

				for (;;)
				{
						if(need_reconstruct){
							HexReconstruct(true);												//sps: Конструируем окно
							need_reconstruct=false;
						}

					if(need_redraw)																//sps: если что-то поменялось - перерисовываем окно
					{

						/////////////////////////////////////////////// БЛОК ПРЕДПРОВЕРКИ КООРДИНАТ КУРСОРА ///////////////////////////////////////////////

						fpoint=(offs+point+shad_cy*hstrsz+shad_cx/2)-1;						//sps: Вычесляем фактическое положение курсора в файле для контроля EOF
						if ((fpoint+hstrsz)<hstrsz){fpoint=0;}								//sps: Работа с первой строкой

						if(fpoint<=(fno->fsize)-1){

						//--------------------------------------
							if(shad_cx < (hex_Lmarg))
							{
								if ((point+offs+shad_cy)!=0)									//sps: Уперлись в начало файла? Никуда не перескакиваем
								{
									shad_cx = (hex_Lmarg+hstrsz*2)-1;							//sps: Уперлись в начало строки? Перескочим на предидущуюю
									shad_cy--;
								}else{shad_cx = hex_Lmarg;}
							}
							//--------------------------------------
							if(shad_cx >= (hex_Lmarg+hstrsz*2))
							{
								shad_cx = hex_Lmarg;											//sps: Уперлись в  конец строки? Перескочим на предидущуюю
								shad_cy++;
							}
							//--------------------------------------
							mcx=shad_cx;
							mcy=shad_cy;
							//--------------------------------------
							if (shad_cy<0){
								shad_cy=0;
								mcy=shad_cy;
								HexScreenUp();													//sps: Листаем вверх
								HexReconstruct(true);											//sps: Конструируем окно
							}else{mcy=shad_cy;}
							//--------------------------------------
							if(shad_cy>(LCD_CLIENT_HEIGHT-2)){
								shad_cy=LCD_CLIENT_HEIGHT-2;
								mcy=shad_cy;
								HexScreenDown();												//sps: Листаем вниз
								HexReconstruct(true);											//sps: Конструируем окно
							}else{mcy=shad_cy;}
							//--------------------------------------
						}else{
							if((offs+point+hstrsz*(LCD_CLIENT_HEIGHT-1))>=(fno->fsize)) 		//sps: Ecле на экране или сразу за ним конец файла
							{
								shad_cx=mcx;													//sps: Запрещаем курсору двигатся дальше
								shad_cy=mcy;
							}else{																//sps: Если нет ->
								HexScreenDown();												//sps: Листаем вниз
								HexReconstruct(true);											//sps: Конструируем окно
																								//sps: Устанавливаем курсор на последний символ
								shad_cx=((((fno->fsize)-offs-point)*2)-hstrsz*2*(LCD_CLIENT_HEIGHT-2))+1;
								shad_cy=LCD_CLIENT_HEIGHT-2;
								mcx=shad_cx;
								mcy=shad_cy;
							}
						}

						/////////////////////////////////////////////// БЛОК ПРЕОБРАЗОВАНИЙ КООРДИНАТ КУРСОРА ///////////////////////////////////////////////

						cursor=LCD_CLIENT_WIDTH*mcy+mcx;									//sps: Вычесляем позицию основного курсора по координатам
						scy=mcy;
						scx=mcx/2;
						slcurs=LCD_CLIENT_WIDTH*scy+(scx+spcount1+hstrsz*2+spcount2);		//sps: Вычесляем позицию вторичного курсора по координатам

						/////////////////////////////////////////////// ФОРМИРУЕМ ВЕРХНЮЮ СТРОКУ СМЕЩЕНИЯ ///////////////////////////////////////////////////

						sprintf(offset,"CURSOR:%08X",offs+point+(scx+(scy*hstrsz))-1);		//sps: получаем OFFSET первой строки в шестнадцтеричном формате

						/////////////////////////////////////////////// БЛОК ОТРИСОВКИ СТРАНИЦЫ /////////////////////////////////////////////////////////////

						sScreen* screen = &window->screen;

						MUTEX_LOCK(window->mutex)											//sps: зажали мютекс окна

							Screen_Clear(screen);
							Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_OPTIONS);

							Screen_PutString(screen,offset,true);

							for(int i=0;i<scrsize-tstrsz;i++)								//sps: Отрисовка окна с курсором
							{
								if(i==cursor || i==slcurs){
									Screen_PutChar(screen,msg[i],true);
								}else{
									Screen_PutChar(screen,msg[i],false);
								}
							}

						MUTEX_UNLOCK(window->mutex)											//sps: отдали мютекс окна

//-----------------------------------------------------------------------------------------------

							need_redraw=false;												//sps: закрыли иф, пока кнопку не ткнут
					}

//-----------------------------------------------------------------------------------------------

				eKey key = LCDUI_Window_FetchKey(window);									//sps: проверяем кнопочки
						if (key != KEY_NONE)
						{
							if (key == KEY_UP || key==KEY_PGUP)
							{
								shad_cy--;
							}
							else if ((key == KEY_DOWN || key == KEY_PGDOWN))
							{
								shad_cy++;
							}
							else if ((key == KEY_LEFT))
							{
								shad_cx--;
							}
							else if ((key == KEY_RIGHT))
							{
								shad_cx++;
							}
							else if (key == KEY_RSOFT)
							{
								return key;
								break;
							}
							else if (key == KEY_LSOFT) {
								if(chestat){				//sps: Если были изменения в буфере, предложим сохранить
									eKey res=LCD_ReadmeWithNoYesButtons(LANG_HEXEDIT_WRASK,JUSTIFY_CENTER);
									if(res==KEY_RSOFT){ReGrab(true);}else{ReGrab(false);}
								}
								return key;
								break;
							}
							else if (key == KEY_PRINT)
							{
								printHex();													//sps: Распечатать окно Hex-просмотрщика

							} else if (key == KEY_0) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'0');
							} else if (key == KEY_1) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'1');
							} else if (key == KEY_2) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'2');
							} else if (key == KEY_3) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'3');
							} else if (key == KEY_4) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'4');
							} else if (key == KEY_5) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'5');
							} else if (key == KEY_6) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'6');
							} else if (key == KEY_7) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'7');
							} else if (key == KEY_8) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'8');
							} else if (key == KEY_9) {	ChangeCHAR(msg,mcx,mcy,scx,scy,'9');
							} else if (key == KEY_00){	ChangeCHAR(msg,mcx,mcy,scx,scy,'X');

							} else {
								beepError();
							}
							need_redraw=true;
							continue;
						}
//-----------------------------------------------------------------------------------------------
						taskYIELD();
				}
				return KEY_NONE;
			}
//================================================================================================
//  SPS ::  ТХТ-просмотрщик
//================================================================================================
			eKey TxtView(sLCDUI_Window* window) {

				sScreen* screen = &window->screen;

					   need_redraw=true;										//sps: Пнуть в ТРУ если нужно перисовать окошко
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
							Screen_DrawButtons(screen,LANG_MENU_BUTTON_BACK,LANG_MENU_BUTTON_OPTIONS);

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

							if (offs+point<=offs && offs!=0)
							{
								point+=hsize;
								offs-=hsize;
								ReGrab(false);
							}
						}
						else if ((key == KEY_DOWN || key == KEY_PGDOWN) && offs+point+scrsize < size)
						{
							point+=tstrsz;

							if (offs+point+scrsize>=offs+wsize)
							{
								point-=hsize;
								offs+=hsize;
								ReGrab(false);
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

							if(chestat){	 //sps: Если были изменения в буфере, предложим сохранить
								eKey res=LCD_ReadmeWithNoYesButtons(LANG_HEXEDIT_WRASK,JUSTIFY_CENTER);
								if(res==KEY_RSOFT){ReGrab(true);}else{ReGrab(false);}
							}
							return key;
							break;
						}
						else if (key == KEY_PRINT)
						{
							printMessage(msg);
						} else {
							beepError();
						}
							need_redraw=true;
							continue;
					}
//-----------------------------------------------------------------------------------------------
						taskYIELD();
				}
					return KEY_NONE;
			}
//================================================================================================
//  SPS ::  File View/Edit menu
//================================================================================================
//-----------------------------------------------------------------------------------------------
			sLCDUI_Window* window = LCDUI_Supervisor_GetMyWindow();

			signed int MenuChose=0;
			eKey rkey;

			ReGrab(false);	//sps: Захватываем первую часть файла в скользящий буфер

			for(;;)
			{
				switch(MenuChose)
				{
					case 0:
							point	=	(point/tstrsz)*tstrsz;				//sps: Уравнитель POINT-a "HEX>>TXT" (Выравниваем точку просмотра по началу строки)
							rkey 	= 	TxtView(window);
							break;
					case 1:
							point	=	(point/hstrsz)*hstrsz;				//sps: Уравнитель POINT-a "TXT>>HEX" (Выравниваем точку просмотра по началу строки)
							rkey 	= 	HexView(window);
							break;
					case 2:
							point	=	(point/hstrsz)*hstrsz;				//sps: Уравнитель POINT-a "TXT>>HEX" (Выравниваем точку просмотра по началу строки)
							rkey 	=	 HexEdit(window);
							break;
				}

				if (rkey==KEY_LSOFT){break;}

				_LCDUI_Form* form = LCDUI_Form_NewUncommon(LANG_MENU_BUTTON_OK,LANG_MENU_BUTTON_CANCEL,false);

				LCDUI_Form_AddStringControl(form,"[FILE OPTIONS]");
				LCDUI_Form_AddControl(form, LCDUI_RadioListItem_New("- Txt Viewer", false, 0, &MenuChose,eInvisible));
				LCDUI_Form_AddControl(form, LCDUI_RadioListItem_New("- Hex Viewer", false, 1, &MenuChose,eInvisible));
				LCDUI_Form_AddControl(form, LCDUI_RadioListItem_New("- Hex Editor", false, 2, &MenuChose,eInvisible));

				LCDUI_Form_Show(form);

				DBGF("MenuChose=%d",MenuChose);

				if (MenuChose==-1)	{break;}

			}

//-----------------------------------------------------------------------------------------------
//================================================================================================
		UNS_FREE(hexstr);
		UNS_FREE(msg);
		UNS_FREE(offset);
		UNS_FREE(hxblok);
		UNS_FREE(asblok);
		UNS_FREE(wbuf);
		return true;
}
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
						//LCDUI_UniversalAwaitScreen(LANG_DISKEXP_HELP, ehjNone, euastNone, euasbCancelOnly, -4000, NULL, NULL, NULL, NULL, NULL);
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
