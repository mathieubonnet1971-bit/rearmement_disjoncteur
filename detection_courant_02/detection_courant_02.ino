/*
 * ========================================================================
 * Système de détection de perte de courant et réarmement automatique
 * ========================================================================
 * 
 * Matériel :
 * - Arduino UNO R4 WiFi
 * - Module GSM SIM900A
 * - Capteur de courant SCT-013-030
 * - Vérin électrique 12V avec pont en H L298N
 * - Bouton poussoir pour test manuel
 * - Batterie 12V pour vérin + chargeur maintien de charge
 * 
 * Fonctionnalités :
 * - Détection perte de courant + SMS alerte
 * - Commande SMS "REARMER" : cycle complet avec résultat
 * - Commande SMS "TEST" : test du cycle
 * - Commande SMS "CONSO" : consommation courante en Watts
 * - Bouton physique pour test manuel
 * 
 * Auteur : Système de réarmement automatique
 * Version : 2.0
 * ========================================================================
 */

#include <SoftwareSerial.h>

// ===== CONFIGURATION PINS =====
#define POWER_SENSE_PIN 14      // Pin A0 - Capteur SCT-013-030
#define MOTOR_IN1 8             // Pin 8 - Contrôle vérin sens 1 (extension)
#define MOTOR_IN2 9             // Pin 9 - Contrôle vérin sens 2 (rétraction)
#define MOTOR_ENA 10            // Pin 10 - Enable moteur (PWM)
#define BUTTON_TEST_PIN 7       // Pin 7 - Bouton poussoir test cycle complet
#define BUTTON_EXTEND_PIN 6     // Pin 6 - Bouton maintenu = extension vérin
#define BUTTON_RETRACT_PIN 5    // Pin 5 - Bouton maintenu = rétraction vérin
#define SIM_RX 0                // Pin 2 - RX SIM900A
#define SIM_TX 1                // Pin 3 - TX SIM900A

// ===== PARAMÈTRES SYSTÈME =====
#define POWER_THRESHOLD 50      // Seuil détection perte courant (amplitude AC)
#define CHECK_INTERVAL 2000     // Intervalle vérification courant (ms)
#define DEBOUNCE_TIME 5000      // Anti-rebond détection perte (ms)
#define ACTUATOR_EXTEND_TIME 15000   // Durée extension vérin (ms)
#define ACTUATOR_RETRACT_TIME 15000  // Durée rétraction vérin (ms)
#define RETRACT_DELAY 2000      // Délai avant rétraction auto (ms)
#define BUTTON_DEBOUNCE 50      // Anti-rebond bouton (ms)

// ===== CALIBRATION SCT-013-030 =====
// Pour un SCT-013-030 : 30A max, sortie 1V
// Tension Arduino : 5V, ADC : 10 bits (0-1023)
// Facteur de calibration : à ajuster selon votre installation
#define VOLTAGE_REF 5.0         // Tension de référence Arduino
#define ADC_COUNTS 1024.0       // Résolution ADC
#define SCT_RATIO 30.0          // Ratio du capteur (30A pour 1V)
#define VOLTAGE_MAINS 230.0     // Tension secteur (230V en France)

// ===== NUMÉRO TÉLÉPHONE =====
const String PHONE_NUMBER = "+33672957874"; // À MODIFIER AVEC VOTRE NUMÉRO

// ===== COMMANDES SMS =====
const String CMD_REARM = "REARMER";
const String CMD_STATUS = "STATUS";
const String CMD_TEST = "TEST";
const String CMD_CONSO = "CONSO";

// ===== VARIABLES GLOBALES =====
bool powerStatus = true;
bool smsSent = false;
bool actuatorExtending = false;
bool actuatorRetracting = false;
bool waitingToRetract = false;
bool rearmInProgress = false;
unsigned long lastCheck = 0;
unsigned long powerLostTime = 0;
unsigned long actuatorStartTime = 0;
unsigned long retractWaitStartTime = 0;
int rearmAttempts = 0;

// Bouton
int lastButtonTestState = HIGH;
int lastButtonExtendState = HIGH;
int lastButtonRetractState = HIGH;
unsigned long lastDebounceTime = 0;
bool manualControl = false;  // Mode contrôle manuel actif

// Communication GSM
SoftwareSerial sim900(SIM_RX, SIM_TX);
String smsBuffer = "";
String mymessage = "";

// Mesure de puissance
float currentRMS = 0.0;
float powerWatts = 0.0;

// Declarations
void stopActuator() {}
void initGSM() {}
void checkPowerStatus() {}
void checkRearmResult() {}
void sendSMS(String message) {}
void retractActuator() {}
void checkButtons() {}
void traiterSMS(String smsBuffer) {}
void processSMS(String smsBuffer) {}

// ===== SETUP =====
void setup() {
  // Initialisation ports série
  Serial.begin(9600);
  sim900.begin(9600);
  
  // Configuration pins
  pinMode(POWER_SENSE_PIN, INPUT);
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);
  pinMode(BUTTON_TEST_PIN, INPUT_PULLUP);
  pinMode(BUTTON_EXTEND_PIN, INPUT_PULLUP);
  pinMode(BUTTON_RETRACT_PIN, INPUT_PULLUP);
  
  // Vérin au repos
  stopActuator();
  
  Serial.println("========================================");
  Serial.println("Systeme de rearmement automatique");
  Serial.println("========================================");
  Serial.println("Initialisation...");
  
  delay(5000); // Attendre démarrage SIM900A
  
  // Initialiser module GSM
  initGSM();
  
  Serial.println("Systeme pret !");
  Serial.println("Commandes SMS disponibles :");
  Serial.println("  - REARMER : Cycle complet de rearmement");
  Serial.println("  - TEST : Test du verin");
  Serial.println("  - CONSO : Consommation courante");
  Serial.println("  - STATUS : Etat du systeme");
  Serial.println("Boutons physiques :");
  Serial.println("  - Bouton TEST : Cycle complet");
  Serial.println("  - Bouton EXTENSION : Maintenir pour etendre");
  Serial.println("  - Bouton RETRACTION : Maintenir pour retracter");
  Serial.println("========================================");
}

// ===== LOOP PRINCIPAL =====
void loop() {
  unsigned long currentTime = millis();
  
  // Vérifier le courant à intervalles réguliers
  if (currentTime - lastCheck >= CHECK_INTERVAL) {
    lastCheck = currentTime;
    checkPowerStatus();
  }
  
  // Gestion du vérin
  if (actuatorExtending && (currentTime - actuatorStartTime >= ACTUATOR_EXTEND_TIME)) {
    stopActuator();
    Serial.println("Extension terminee");
    
    // Vérifier si réarmement réussi
    if (rearmInProgress) {
      delay(500); // Laisser le temps au disjoncteur de réagir
      checkRearmResult();
    }
  }
  
  if (actuatorRetracting && (currentTime - actuatorStartTime >= ACTUATOR_RETRACT_TIME)) {
    stopActuator();
    Serial.println("Retraction terminee - Position initiale");
    if (rearmInProgress) {
      rearmInProgress = false;
      sendSMS("Cycle termine. Verin en position initiale.");
    }
  }
  
  if (waitingToRetract && (currentTime - retractWaitStartTime >= RETRACT_DELAY)) {
    waitingToRetract = false;
    retractActuator();
  }
  
  // Vérifier bouton poussoir
  checkButtons();
  
  // Lire SMS entrants
  if (sim900.available()) {
    char c = sim900.read();
    Serial.write(c);
    smsBuffer += c;

        // DEBUG: Afficher l'état du buffer
    if (c == '\n') {
      Serial.print("[Newline count: ");
      int count = 0;
      for (int i = 0; i < smsBuffer.length(); i++) {
        if (smsBuffer[i] == '\n') count++;
      }
      Serial.print(count);
      Serial.println("]");
    }
    
    // Détecter SMS : +CMT présent ET on a reçu au moins 2 retours à la ligne après
    if (smsBuffer.indexOf("+CMT:") >= 0) {
      // Compter les retours à la ligne après +CMT
      int cmtPos = smsBuffer.indexOf("+CMT:");
      String afterCMT = smsBuffer.substring(cmtPos);
      int newlineCount = 0;
      for (int i = 0; i < afterCMT.length(); i++) {
        if (afterCMT[i] == '\n') newlineCount++;
      }
      
      // Si on a au moins 2 retours à la ligne après +CMT, le SMS est complet
      if (newlineCount >= 2) {
        Serial.println("\n=== SMS COMPLET DETECTE ===");
        traiterSMS(smsBuffer);
        smsBuffer = ""; // Vider le buffer
      }
    }
    
    // Limiter la taille du buffer (sécurité)
    if (smsBuffer.length() > 500) {
      Serial.println("\n!!! BUFFER TROP GRAND, VIDAGE !!!");
      smsBuffer = "";
    }
  }
  
  // Commandes AT manuelles depuis le moniteur série
  while (Serial.available()) {
    char c = Serial.read();
    sim900.write(c);
  }
    processSMS(smsBuffer);
    smsBuffer = "";
    if (smsBuffer.indexOf("+CMT:") >= 0 && smsBuffer.indexOf("\n\n") >= 0) {
      processSMS(smsBuffer);
      smsBuffer = "";
    }
  
}

// ===== DÉTECTION COURANT =====
checkPowerStatus() {
  // Mesure du courant RMS sur plusieurs cycles AC
  int maxValue = 0;
  int minValue = 1023;
  long sum = 0;
  int samples = 0;
  
  // Échantillonner pendant 200ms (10 cycles à 50Hz)
  unsigned long startTime = millis();
  while (millis() - startTime < 200) {
    int sensorValue = analogRead(POWER_SENSE_PIN);
    sum += sensorValue;
    samples++;
    if (sensorValue > maxValue) maxValue = sensorValue;
    if (sensorValue < minValue) minValue = sensorValue;
    delayMicroseconds(100);
  }
  
  // Calculer amplitude et moyenne
  int amplitude = maxValue - minValue;
  int moyenne = sum / samples;
  
  // Calculer courant RMS (approximation)
  float voltage = (amplitude / 2.0) * (VOLTAGE_REF / ADC_COUNTS);
  currentRMS = voltage * SCT_RATIO;
  powerWatts = currentRMS * VOLTAGE_MAINS;
  
  // Affichage debug
  Serial.print("Amplitude: ");
  Serial.print(amplitude);
  Serial.print(" | Courant: ");
  Serial.print(currentRMS, 2);
  Serial.print("A | Puissance: ");
  Serial.print(powerWatts, 0);
  Serial.println("W");
  
  // Détection perte de courant
  if (amplitude < POWER_THRESHOLD && powerStatus) {
    powerLostTime = millis();
    powerStatus = false;
    Serial.println("*** PERTE DE COURANT DETECTEE ! ***");
  }
  
  // Envoi SMS après anti-rebond
  if (!powerStatus && !smsSent && (millis() - powerLostTime > DEBOUNCE_TIME)) {
    sendPowerOutageAlert();
    smsSent = true;
  }
  
  // Détection retour courant
  if (amplitude >= POWER_THRESHOLD && !powerStatus) {
    powerStatus = true;
    Serial.println("*** COURANT RETABLI ***");
    
    // Si réarmement en cours
    if (rearmInProgress && actuatorExtending) {
      stopActuator();
      Serial.println("Rearmement REUSSI !");
      waitingToRetract = true;
      retractWaitStartTime = millis();
      rearmAttempts = 0;
    }
    
    // SMS rétablissement
    if (smsSent && !rearmInProgress) {
      sendPowerRestoredAlert();
      smsSent = false;
    }
  }
}

// ===== VÉRIFICATION RÉSULTAT RÉARMEMENT =====
void checkRearmResult() {
  delay(1000); // Attendre stabilisation
  
  // Refaire une mesure
  int maxValue = 0;
  int minValue = 1023;
  unsigned long startTime = millis();
  while (millis() - startTime < 100) {
    int sensorValue = analogRead(POWER_SENSE_PIN);
    if (sensorValue > maxValue) maxValue = sensorValue;
    if (sensorValue < minValue) minValue = sensorValue;
  }
  int amplitude = maxValue - minValue;
  
  if (amplitude >= POWER_THRESHOLD) {
    // Succès
    Serial.println("RESULTAT : COURANT RETABLI");
    sendSMS("COURANT RETABLI ! Retraction du verin...");
    waitingToRetract = true;
    retractWaitStartTime = millis();
    powerStatus = true;
    smsSent = false;
    rearmAttempts = 0;
  } else {
    // Échec
    Serial.println("RESULTAT : PERTE DE COURANT DETECTEE !");
    sendSMS("PERTE DE COURANT DETECTEE ! Rearmement echoue. Tentative " + String(rearmAttempts) + ". Renvoyez REARMER pour reessayer.");
    retractActuator(); // Rétracter quand même
  }
}

// ===== GESTION BOUTONS =====
void checkButtons() {
  // Bouton TEST (cycle complet)
  int readingTest = digitalRead(BUTTON_TEST_PIN);
  
  if (readingTest != lastButtonTestState) {
    lastDebounceTime = millis();
  }
  
  if ((millis() - lastDebounceTime) > BUTTON_DEBOUNCE) {
    if (readingTest == LOW && lastButtonTestState == HIGH) {
      Serial.println("BOUTON TEST PRESSE - Lancement cycle test");
      handleTestCycle();
    }
  }
  
  lastButtonTestState = readingTest;
  
  // Boutons EXTENSION et RÉTRACTION (contrôle maintenu)
  int readingExtend = digitalRead(BUTTON_EXTEND_PIN);
  int readingRetract = digitalRead(BUTTON_RETRACT_PIN);
  
  // Priorité : si un cycle automatique est en cours, ignorer les boutons manuels
  if (rearmInProgress) {
    return;
  }
  
  // Bouton EXTENSION maintenu
  if (readingExtend == LOW) {
    if (!manualControl || !actuatorExtending) {
      Serial.println("BOUTON EXTENSION MAINTENU");
      manualControl = true;
      extendActuator();
    }
  }
  // Bouton RÉTRACTION maintenu
  else if (readingRetract == LOW) {
    if (!manualControl || !actuatorRetracting) {
      Serial.println("BOUTON RETRACTION MAINTENU");
      manualControl = true;
      retractActuator();
    }
  }
  // Aucun bouton maintenu - arrêter le vérin si en mode manuel
  else if (manualControl && (actuatorExtending || actuatorRetracting)) {
    Serial.println("BOUTON RELACHE - Arret verin");
    stopActuator();
    manualControl = false;
  }
  
  lastButtonExtendState = readingExtend;
  lastButtonRetractState = readingRetract;
}

// ===== INITIALISATION GSM =====
void initGSM() {
  Serial.println("Initialisation module GSM...");
  
  sim900.println("AT");
  delay(1000);
  clearGSMBuffer();
  
  sim900.println("AT+CMGF=1");
  delay(1000);
  clearGSMBuffer();
  
  sim900.println("AT+CSCS=\"GSM\"");
  delay(1000);
  clearGSMBuffer();
  
  sim900.println("AT+CNMI=2,2,0,0,0");
  delay(1000);
  clearGSMBuffer();
  
  sim900.println("AT+CSQ");
  delay(1000);
  clearGSMBuffer();
  
  sim900.println("AT+CMGD=1,4");
  delay(2000);
  clearGSMBuffer();
  
  Serial.println("Module GSM initialise !");
}

// ===== TRAITEMENT SMS =====
void processSMS(String sms) {
  Serial.println("SMS recu :");
  Serial.println(sms);
  
  // Extraire numéro expéditeur
  int phoneStart = sms.indexOf("+CMT: \"") + 7;
  int phoneEnd = sms.indexOf("\"", phoneStart);
  String senderNumber = sms.substring(phoneStart, phoneEnd);
  
  // Extraire message
  int msgStart = sms.indexOf("\n", phoneEnd) + 1;
  String message = sms.substring(msgStart);
  message.trim();
  message.toUpperCase();
  
  Serial.print("De : ");
  Serial.println(senderNumber);
  Serial.print("Message : ");
  Serial.println(message);
  
  // Vérifier numéro autorisé
  if (senderNumber.indexOf(PHONE_NUMBER.substring(3)) < 0) {
    Serial.println("Numero non autorise !");
    return;
  }
  
  // Traiter commandes
  if (message.indexOf("REARMER") >= 0) {
    Serial.println("Commande REARMER detectee !");
    handleRearmCommand();
  }
  else if (message.indexOf(CMD_CONSO) >= 0) {
    handleConsoCommand();
  }
  else if (message.indexOf(CMD_STATUS) >= 0) {
    handleStatusCommand();
  }
  else if (message.indexOf(CMD_TEST) >= 0) {
    handleTestCommand();
  }
  else {
    sendSMS("Commande non reconnue. Commandes : REARMER, TEST, CONSO, STATUS");
  }
}

// ===== COMMANDE RÉARMER =====
void handleRearmCommand() {
  Serial.println(">>> COMMANDE REARMER <<<");
  
  if (actuatorExtending || actuatorRetracting) {
    sendSMS("Verin deja en operation, veuillez patienter");
    return;
  }
  
  if (powerStatus) {
    sendSMS("Le courant est present, rearmement non necessaire");
    return;
  }
  
  rearmAttempts++;
  rearmInProgress = true;
  sendSMS("Tentative " + String(rearmAttempts) + " : Extension du verin...");
  extendActuator();
}

// ===== COMMANDE CONSO =====
void handleConsoCommand() {
  Serial.println(">>> COMMANDE CONSO <<<");
  
  String msg = "Consommation courante :\n";
  msg += "Courant : " + String(currentRMS, 2) + " A\n";
  msg += "Puissance : " + String(powerWatts, 0) + " W\n";
  msg += "Etat : " + String(powerStatus ? "Normal" : "Perte courant");
  
  sendSMS(msg);
}

// ===== COMMANDE STATUS =====
void handleStatusCommand() {
  Serial.println(">>> COMMANDE STATUS <<<");
  
  String status = "Etat systeme :\n";
  status += "Courant : ";
  status += powerStatus ? "Present" : "Absent";
  status += "\nVerin : ";
  if (actuatorExtending) status += "Extension";
  else if (actuatorRetracting) status += "Retraction";
  else status += "Repos";
  status += "\nConso : " + String(powerWatts, 0) + "W";
  status += "\nTentatives : " + String(rearmAttempts);
  
  sendSMS(status);
}

// ===== COMMANDE TEST =====
void handleTestCommand() {
  Serial.println(">>> COMMANDE TEST (SMS) <<<");
  
  if (actuatorExtending || actuatorRetracting) {
    sendSMS("Test impossible, verin actif");
    return;
  }
  
  handleTestCycle();
}

// ===== CYCLE TEST =====
void handleTestCycle() {
  Serial.println(">>> CYCLE TEST COMPLET <<<");
  
  sendSMS("Test : Extension verin...");
  extendActuator();
  delay(ACTUATOR_EXTEND_TIME + 500);
  
  stopActuator();
  delay(1000);
  
  sendSMS("Test : Retraction verin...");
  retractActuator();
  delay(ACTUATOR_RETRACT_TIME + 500);
  
  stopActuator();
  sendSMS("Test termine. Verin en position initiale.");
}

// ===== CONTRÔLE VÉRIN =====
void extendActuator() {
  Serial.println(">>> EXTENSION VERIN <<<");
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 255); // Vitesse max
  actuatorExtending = true;
  actuatorRetracting = false;
  actuatorStartTime = millis();
}

void retractActuator() {
  Serial.println(">>> RETRACTION VERIN <<<");
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  analogWrite(MOTOR_ENA, 255); // Vitesse max
  actuatorRetracting = true;
  actuatorExtending = false;
  actuatorStartTime = millis();
}

void stopActuator() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 0);
  
  // Ne réinitialiser les flags que si pas en mode manuel
  if (!manualControl) {
    actuatorExtending = false;
    actuatorRetracting = false;
  }
}

// ===== ENVOI SMS =====
void sendPowerOutageAlert() {
  Serial.println("Envoi SMS alerte perte courant...");
  
  sim900.print("AT+CMGS=\"");
  sim900.print(PHONE_NUMBER);
  sim900.println("\"");
  delay(1000);
  
  sim900.print("ALERTE : Perte de courant detectee !\n");
  sim900.print("Repondez REARMER pour tenter le rearmement.\n");
  sim900.print("CONSO pour la consommation, STATUS pour l'etat.");
  delay(500);
  
  sim900.write(26);
  delay(5000);
  clearGSMBuffer();
  
  Serial.println("SMS alerte envoye !");
}

void sendPowerRestoredAlert() {
  Serial.println("Envoi SMS retablissement...");
  
  sim900.print("AT+CMGS=\"");
  sim900.print(PHONE_NUMBER);
  sim900.println("\"");
  delay(1000);
  
  sim900.print("INFO : Courant retabli.\n");
  sim900.print("Duree coupure : ");
  sim900.print((millis() - powerLostTime) / 1000);
  sim900.println(" secondes");
  delay(500);
  
  sim900.write(26);
  delay(5000);
  clearGSMBuffer();
  
  Serial.println("SMS retablissement envoye !");
}

void sendSMS(String message) {
  Serial.println("Envoi SMS : " + message);
  
  sim900.print("AT+CMGS=\"");
  sim900.print(PHONE_NUMBER);
  sim900.println("\"");
  delay(1000);
  
  sim900.println(message);
  delay(500);
  
  sim900.write(26);
  delay(5000);
  clearGSMBuffer();
  
  Serial.println("SMS envoye !");
}

void clearGSMBuffer() {
  delay(500);
  while (sim900.available()) {
    sim900.read();
  }
}

