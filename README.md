# h264-editing-utils  
**Коллекция утилит для работы с H.264.**  

Проект нацелен на создание инструментов для точной и эффективной работы с видеопотоками H.264.

## Основные цели проекта
1. **Упрощённая обработка H.264:**  
   + Вырезание фрагментов с минимальным перекодированием.  
   + Поддержка миллисекундной точности при обработке видео.  

2. **Удобство для разработчиков и пользователей:**  
   + Исходный код имеет чёткую структуру, где всё упорядочено.  
   + Стандарты хранения файлов в папках:  
     - Исходный код располагается исключительно в папке `source`.  
     - Все данные и утилиты разделены по функциональным папкам.
3. **Приверженность свободному ПО:**  
   + Проект полностью соответствует принципам GNU и распространяется под свободными лицензиями.  

## Используемая библиотека
Наверное, за основу возьмётся свободная библиотека [h264bitstream](https://github.com/aizvorski/h264bitstream).  
Потому что:  
  а. Она свободная (*GNU Lesser General Public License*);  
  б. Написана на Си;  


## Планы на будущее
1. Изучение и конспектирование документации стандарта H.264.  
2. Разработка программы-утилиты, способной работать с видео-аудио-контейнерами (в долгосрочной перспективе).  

## Структура репозитория  
- `source/` — исходный код, включая утилиты и библиотечные файлы.  
- `docs/` — документация и конспекты по работе с H.264.    

## Лицензия  
- Весь исходный код распространяется под лицензией **GPLv3 or later**.  
- Документация распространяется под **GNU Free Documentation License 1.3**.  
[Подробнее](./COPYING).

