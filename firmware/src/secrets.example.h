// רשת הבית ופרטי הכניסה, נקבעים לפני הצריבה (לא חובה).
//
// 1. העתק את הקובץ הזה לקובץ בשם secrets.h, באותה תיקייה.
// 2. מלא את הערכים שלך.
// 3. בנה וצרוב עם PlatformIO.
//
// המכשיר יתחבר לרשת הבית כבר בהפעלה הראשונה, בלי לעבור דרך הרשת HElectricity.
// secrets.h ברשימת ההתעלמות של git (‎.gitignore) ולא יעלה ל-GitHub.
// אל תשתף את קובץ ה-firmware.bin שתבנה ממנו: הערכים שכאן נמצאים בתוכו.
//
// הערכים משמשים רק כשאין למכשיר הגדרות משלו: בהפעלה הראשונה, ושוב אחרי
// איפוס עם כפתור BOOT (לחיצה של 10 שניות). שינויים מהממשק גוברים עליהם.
#pragma once

// רשת הבית (2.4GHz בלבד)
#define PRESET_WIFI_SSID "MyHomeWiFi"
#define PRESET_WIFI_PASSWORD "my-wifi-password"

// לא חובה: שם המכשיר ברשת, לכתובת http://<השם>.local
// אותיות באנגלית, ספרות ומקף בלבד
// #define PRESET_HOSTNAME "electricity-meter"

// לא חובה: שם משתמש וסיסמה לממשק (סיסמה של 8 תווים לפחות).
// בלעדיהם, הכניסה הראשונה לממשק תבקש ליצור משתמש.
// #define PRESET_ADMIN_USER "admin"
// #define PRESET_ADMIN_PASSWORD "choose-a-password"
