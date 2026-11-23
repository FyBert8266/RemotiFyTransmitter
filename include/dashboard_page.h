/**
 * @file dashboard_page.h
 * @version 1.2.4
 * @brief Header-Datei fuer das Dashboard des Fahrbetriebs (Optimiert fuer CYD 480x320, LVGL 9).
 * @Version_info v1.2.4: Deklaration on_active_model_changed_ui_hook(int newModelIndex) ergaenzt.
 *               v1.2.3: Schnittstellen fuer Winden-Sperrstatus, 50%-Button-Auslenkung, Hardware-IO-Feedback und Blockade-Rahmenblitz ergaenzt.
 */

#ifndef DASHBOARD_PAGE_H
#define DASHBOARD_PAGE_H

#include <Arduino.h>
#include <lvgl.h>

/**
 * @brief Erstellt und initialisiert den Dashboard-Tab im uebergebenen Tab-Container.
 * @param parent_tab Zeiger auf den Ziel-Tab der Tabview.
 */
void build_dashboard_tab(lv_obj_t *parent_tab);

/**
 * @brief Raeumt den Speicher des Dashboard-Tabs beim Verlassen vollstaendig ab.
 */
void cleanup_dashboard_tab(void);

/**
 * @brief Aktualisiert dynamische Live-Daten (Beleuchtung, Pitch/Roll, Winde, Gaenge) im Haupt-Loop.
 */
void update_dashboard_ui(void);

/**
 * @brief Erneuert die dynamische Gang-Schaltleiste aus der Kanaldefinition (GEARBOX states).
 */
void rebuild_dynamic_gear_bar(void);

/**
 * @brief Hook bei Wechsel des aktiven Modells aus espnow_comm.cpp (via lv_async_call).
 * @param newModelIndex Neuer aktiver Modell-Index (-1 = kein Modell aktiv).
 */
void on_active_model_changed_ui_hook(int newModelIndex);

/**
 * @brief Liefert zurueck, ob der Winden-Sperrschalter aktiv ist (true = Kanal auf 1500 us fixiert).
 * @return true, wenn die Winde gesperrt ist.
 */
bool is_winch_locked(void);

/**
 * @brief Liefert den aktuellen Ziel-PWM-Wert (1000..2000 us) aus den Winden-Buttons (50% Richtungs-Limit).
 * @param winch_channel_idx Index des WINCH-Kanals in currentConfig.channels.
 * @return Ziel-PWM in Mikrosekunden (1500 us wenn kein Button aktiv ist).
 */
uint16_t get_winch_button_target_pwm(int winch_channel_idx);

/**
 * @brief Liefert den aktuellen Roh-/PWM-Wert (1000..2000 us) eines Hardware-Gebers fuer den WINCH-Kanal.
 * @param winch_channel_idx Index des WINCH-Kanals in currentConfig.channels.
 * @return PWM-Wert in Mikrosekunden (1500 us wenn kein Hardware-Geber zugewiesen oder innerhalb Deadband).
 */
uint16_t get_winch_hardware_pwm(int winch_channel_idx);

#endif // DASHBOARD_PAGE_H