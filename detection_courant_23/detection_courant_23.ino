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
 * Auteur : MBO
 * Version : 1.0
 * ========================================================================
 */

// #include <SoftwareSerial.h>
#define sim900 Serial1

const String version = "1.0.0";

// === PROTOTYPES ===
bool gsmAlive();
void gsmHardReset();


// Pins pour SIM900A
#define SIM_RX 0  // 0 ou 7
#define SIM_TX 1  // 1 ou 8
#define SIM900_PWRKEY 4
#define POWER_SENSE_PIN A0    // Pin A0 - Capteur SCT-013-030
#define MOTOR_IN1 12          // Pin 12 - Contrôle vérin sens 1 (extension)
#define MOTOR_IN2 11          // Pin 11 - Contrôle vérin sens 2 (rétraction)
#define MOTOR_ENA 10          // Pin 10 - Enable moteur (PWM)
#define BUTTON_TEST_PIN 7     // Pin 7 - Bouton poussoir test cycle complet
#define BUTTON_EXTEND_PIN 6   // Pin 6 - Bouton maintenu = extension vérin
#define BUTTON_RETRACT_PIN 5  // Pin 5 - Bouton maintenu = rétraction vérin
#define GSM_RESET_PIN 6       // Watchdog GSM pin


#define POWER_THRESHOLD 10  // Seuil détection perte courant (amplitude AC)
#define DEBOUNCE_TIME 5000    // Anti-rebond détection perte (ms)

#define ACTUATOR_EXTEND_TIME 15000   // Durée extension vérin (ms)
#define ACTUATOR_RETRACT_TIME 15000  // Durée rétraction vérin (ms)
#define RETRACT_DELAY 2000           // Délai avant rétraction auto (ms)

#define VOLTAGE_REF 5.0      // Tension de référence Arduino
#define ADC_COUNTS 1024.0    // Résolution ADC
#define SCT_RATIO 30.0       // Ratio du capteur (30A pour 1V)
#define VOLTAGE_MAINS 230.0  // Tension secteur (230V en France)
#define CHECK_INTERVAL 5000  // Intervalle vérification courant (ms)


float currentRMS = 0.0;
float powerWatts = 0.0;
float lastPowerBeforeCut = 0;
float lastCurrentBeforeCut = 0;

bool smsSent = false;
unsigned long powerLostTime = 0;
bool powerStatus = true;
bool rearmInProgress = false;
bool actuatorExtending = false;
bool waitingToRetract = false;
unsigned long retractWaitStartTime = 0;
int rearmAttempts = 0;
unsigned long lastCheck = 0;
//bool actuatorExtending = false;
bool actuatorRetracting = false;
//bool waitingToRetract = false;
//bool rearmInProgress = false;
//unsigned long lastCheck = 0;
//unsigned long powerLostTime = 0;
unsigned long actuatorStartTime = 0;
//unsigned long retractWaitStartTime = 0;
//int rearmAttempts = 0;

unsigned long GSMlastCheck = 0;
const unsigned long CHECK_INTERVAL_GSM = 30000;  // 30 s
const unsigned long GSM_TIMEOUT = 2000;          // 2 s

// Bouton
int lastButtonTestState = HIGH;
int lastButtonExtendState = HIGH;
int lastButtonRetractState = HIGH;
unsigned long lastDebounceTime = 0;
bool manualControl = false;  // Mode contrôle manuel actif


// ===== COMMANDES SMS =====
const String CMD_REARM = "REARMER";
const String CMD_STATUS = "STATUS";
const String CMD_TEST = "TEST";
const String CMD_CONSO = "CONSO";

const unsigned long uptime = 0;
unsigned long lastPrint = 0;

bool started = false;

// Votre numéro de téléphone (format international)
const String MY_PHONE = "+33672957874";  // CHANGEZ CECI !

// Communication série
// SoftwareSerial sim900(SIM_RX, SIM_TX);

String smsBuffer = "";

void setup() {
  analogReadResolution(10);
  pinMode(GSM_RESET_PIN, OUTPUT);
  digitalWrite(GSM_RESET_PIN, HIGH);

  // Configuration des ports en mode "sortie"
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);

  // Moniteur série pour debug
  Serial.begin(9600);

  pinMode(POWER_SENSE_PIN, INPUT);

  Serial.println("=============================================");
  Serial.println("      DETECTEUR DE PERTE COURANT STARTED     ");
  Serial.println("=============================================");

  Serial.println("=============================================");
  Serial.println("Initialisation module GSM...");
  delay(3000);

  pinMode(SIM900_PWRKEY, OUTPUT);
  digitalWrite(SIM900_PWRKEY, LOW);

  // Communication avec SIM900A (utiliser le bon baud rate trouvé)
  sim900.begin(9600);  // Ajustez selon le résultat ci-dessus

  ensureSim900On();

  // Test communication
  sim900.println("AT");
  delay(1000);
  afficherReponse();

  // Mode texte pour SMS
  sim900.println("AT+CMGF=1");
  delay(1000);
  afficherReponse();

  // Activer notification SMS entrants
  sim900.println("AT+CNMI=2,2,0,0,0");
  delay(1000);
  afficherReponse();

  // Supprimer anciens SMS
  sim900.println("AT+CMGD=1,4");
  delay(2000);
  afficherReponse();

  Serial.println("=============================================");
  Serial.println("Module GSM pret !");
  Serial.println("Envoyez un SMS avec une commande reconnue");
  Serial.println("=============================================");
}

void loop() {

  unsigned long currentTime = millis();

  //if (millis() - lastPrint > 60000) { // toutes les 60 s
  //  lastPrint = millis();
  //  printUptime();
  //}

  if (millis() - GSMlastCheck > CHECK_INTERVAL_GSM) {
    GSMlastCheck = millis();

    if (!gsmAlive()) {
      gsmHardReset();
    }
  }

  // Vérifier le courant à intervalles réguliers
  if (currentTime - lastCheck >= CHECK_INTERVAL) {
    lastCheck = currentTime;
    checkPowerStatus();
  }

  // Lire données du SIM900A
  while (sim900.available()) {
    char c = sim900.read();
    Serial.write(c);  // Afficher dans le moniteur série
    smsBuffer += c;

    // DEBUG: Afficher l'état du buffer
    // if (c == '\n') {
    //   Serial.print("[Newline count: ");
    //   int count = 0;
    //   for (int i = 0; i < smsBuffer.length(); i++) {
    //     if (smsBuffer[i] == '\n') count++;
    //   }
    //   Serial.print(count);
    //   Serial.println("]");
    // }

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
        smsBuffer = "";  // Vider le buffer
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
}

void traiterSMS(String sms) {
  Serial.println("SMS recu :");
  Serial.println(sms);
  Serial.println("-------------------");

  // Extraire le numéro de l'expéditeur
  int start = sms.indexOf("+CMT: \"") + 7;
  int end = sms.indexOf("\"", start);
  String numero = sms.substring(start, end);

  // Extraire le message
  int msgStart = sms.indexOf("\n", end) + 1;
  String message = sms.substring(msgStart);
  message.trim();
  message.toUpperCase();  // Convertir en majuscules

  Serial.print("Expediteur: ");
  Serial.println(numero);
  Serial.print("Message: ");
  Serial.println(message);

  // Répondre si le message contient "REARMER"
  if (message.indexOf(CMD_REARM) >= 0) {
    Serial.println("Commande " + CMD_REARM + " detectée !");
    envoyerSMS(MY_PHONE, "Bonjour !\nMessage '" + CMD_REARM + "' recu.\nRearmement en cours : extension verin...");
    extendActuator();
    delay(ACTUATOR_EXTEND_TIME + 500);

    stopActuator();
    delay(1000);

    envoyerSMS(MY_PHONE, "Rearmement en cours : retraction verin...");
    retractActuator();
    delay(ACTUATOR_RETRACT_TIME + 500);

    stopActuator();
    envoyerSMS(MY_PHONE, "Rearmement termine : verin en position initiale.");
    handleConsoCommand();
  } else if (message.indexOf("TEST") >= 0) {
    Serial.println("Commande TEST detectee !");
    envoyerSMS(MY_PHONE, "Test OK ! Le module fonctionne correctement. Firmware version: "+version);
  } else if (message.indexOf("STATUS") >= 0) {
    Serial.println("Commande STATUS detectee !");
    // envoyerSMS(MY_PHONE, "Systeme actif. Temps: " + String(millis()/1000) + "s");
    sendUptimeSMS();
  } else if (message.indexOf("CONSO") >= 0) {
    Serial.println("Commande CONSO detectee !");
    //envoyerSMS(numero, "Consommation actuelle: "+currentPower+"W / "+currentAmps+"A");
    //String currentPowerDetected = checkPowerStatus();
    //Serial.println(currentPowerDetected);
    handleConsoCommand();
  } else {
    Serial.println("Message non reconnu");
    envoyerSMS(MY_PHONE, "Commande '" + message + "' non reconnue.\nCommandes disponibles:\n'" + CMD_REARM + "', '" + CMD_STATUS + "', '" + CMD_TEST + "', '" + CMD_CONSO + "'");
  }
}

void envoyerSMS(String numero, String message) {
  Serial.println("Envoi SMS...");
  Serial.print("Vers: ");
  Serial.println(numero);
  Serial.print("Message: ");
  Serial.println(message);

  // Commande d'envoi de SMS
  sim900.print("AT+CMGS=\"");
  sim900.print(numero);
  sim900.println("\"");
  delay(1000);

  // Contenu du message
  sim900.print(message);
  delay(500);

  // Caractère Ctrl+Z (26) pour envoyer
  sim900.write(26);
  delay(5000);

  afficherReponse();
  Serial.println("SMS envoye !");
}

void afficherReponse() {
  delay(500);
  while (sim900.available()) {
    Serial.write(sim900.read());
  }
}

// ===== DÉTECTION COURANT =====
void checkPowerStatus() {
  // Mesure du courant RMS sur plusieurs cycles AC
  int maxValue = 0;
  int minValue = 1023;
  long sum = 0;
  int samples = 0;

  // // Échantillonner pendant 200ms (10 cycles à 50Hz)
  // unsigned long startTime = millis();
  // while (millis() - startTime < 200) {
  //   int sensorValue = analogRead(POWER_SENSE_PIN);
  //   sum += sensorValue;
  //   samples++;
  //   if (sensorValue > maxValue) maxValue = sensorValue;
  //   if (sensorValue < minValue) minValue = sensorValue;
  //   delayMicroseconds(100);
  // }

  // Calculer amplitude et moyenne
  // int amplitude = maxValue - minValue;
  // int amplitude = abs(maxValue - minValue);
  int moyenne = sum / samples;

  // // Calculer courant RMS (approximation)
  // float voltage = (amplitude / 2.0) * (VOLTAGE_REF / ADC_COUNTS);
  // currentRMS = voltage * SCT_RATIO;
  // powerWatts = currentRMS * VOLTAGE_MAINS;

  // long sumSquares = 0;
  // // int samples = 0;

  // // Offset (important !)
  // const int offset = 512;

  // unsigned long startTime = millis();
  // while (millis() - startTime < 200) {
  //   int sensorValue = analogRead(POWER_SENSE_PIN);

  //   // int centered = sensorValue - offset;   // recentrer autour de 0
  //   // sumSquares += (long)centered * centered;

  //   int centered = sensorValue - offset;
  //   sumSquares += (long)centered * centered;

  //   samples++;
  //   delayMicroseconds(100);
  // }

  // // RMS ADC
  // float meanSquares = (float)sumSquares / samples;
  // float rmsADC = sqrt(meanSquares);

  // // Conversion en tension
  // float voltage = rmsADC * (VOLTAGE_REF / ADC_COUNTS);

  // // Conversion courant
  // currentRMS = voltage * SCT_RATIO;


  // long sum = 0;
  long long sumSquares = 0;
  // int samples = 0;

  unsigned long startTime = millis();

  while (millis() - startTime < 200) {
    int val = analogRead(POWER_SENSE_PIN);

    sum += val;
    samples++;

    delayMicroseconds(100);
  }

  // 1️⃣ calcul offset réel
  float offset = (float)sum / samples;

  // 2️⃣ recalcul RMS propre
  startTime = millis();
samples = 0;
sumSquares = 0;

maxValue = 0;
minValue = 1023;

while (millis() - startTime < 200) {
  int val = analogRead(POWER_SENSE_PIN);

  if (val > maxValue) maxValue = val;
  if (val < minValue) minValue = val;

  float centered = val - offset;

  sumSquares += (long long)centered * centered;

  samples++;

  delayMicroseconds(100);
}

int amplitude = maxValue - minValue;

if (samples == 0) return;

float rmsADC = sqrt((float)sumSquares / samples);

  // conversion
  float voltage = rmsADC * (VOLTAGE_REF / ADC_COUNTS);
  currentRMS = voltage * SCT_RATIO;

  // seuil anti-bruit
  if (currentRMS < 0.05) currentRMS = 0;

  // Serial.print("DEBUG power calc: ");
  // Serial.println(currentRMS * VOLTAGE_MAINS);
  powerWatts = currentRMS * VOLTAGE_MAINS;

  // Affichage debug
  Serial.print("Min: ");
  Serial.print(minValue);
  Serial.print(" | Max: ");
  Serial.print(maxValue);
  Serial.print(" | Amplitude: ");
  Serial.print(amplitude);
  Serial.print(" | Courant: ");
  Serial.print(currentRMS, 2);
  Serial.print("A | Puissance: ");
  Serial.print(powerWatts, 0);
  Serial.println("W");


  if (amplitude < POWER_THRESHOLD) {
    lastPowerBeforeCut = powerWatts;
    lastCurrentBeforeCut = currentRMS;
  }

  // Détection perte de courant
  if (amplitude < POWER_THRESHOLD && powerStatus) {
    powerLostTime = millis();
    powerStatus = false;
    Serial.println("POWER_THRESHOLD && powerStatus: ");
    Serial.println(POWER_THRESHOLD && powerStatus);
    Serial.println("*** PERTE DE COURANT DETECTEE ! ***");
  }

  // Envoi SMS après anti-rebond
  if (!powerStatus && !smsSent && (millis() - powerLostTime > DEBOUNCE_TIME)) {
    //sendPowerOutageAlert();
    char bufPower[64];
    char bufAmps[64];
    char sms[80];

    dtostrf(lastPowerBeforeCut, 6, 0, bufPower);   // ex "  742"
    dtostrf(lastCurrentBeforeCut, 5, 2, bufAmps);  // ex " 3.24"

    sprintf(sms,
            "/!\\ PERTE DE COURANT!\nConsommation avant coupure:\n     • %s W\n     • %s A\nRearmer?",
            bufPower,
            bufAmps);

    envoyerSMS(MY_PHONE, sms);
    smsSent = true;
    //return currentPower;
  }

  // Détection retour courant
  if (amplitude >= POWER_THRESHOLD && !powerStatus) {
    powerStatus = true;
    Serial.println("*** COURANT RETABLI ***");

    // Si réarmement en cours
    if (rearmInProgress && actuatorExtending) {
      //stopActuator();       //MBO
      Serial.println("Rearmement REUSSI !");
      waitingToRetract = true;
      retractWaitStartTime = millis();
      rearmAttempts = 0;
    }

    // SMS rétablissement
    if (smsSent && !rearmInProgress) {
      //sendPowerRestoredAlert();   //MBO
      smsSent = false;
    }
  }
}

void handleConsoCommand() {
  //if (!powerStatus && !smsSent && (millis() - powerLostTime > DEBOUNCE_TIME)) {
  char bufPower[10];
  char bufAmps[10];
  char sms[80];

  dtostrf(powerWatts, 6, 0, bufPower);
  dtostrf(currentRMS, 5, 2, bufAmps);

  sprintf(sms,
          "Consommation actuelle:\n     • %s W\n     • %s A",
          bufPower,
          bufAmps);

  envoyerSMS(MY_PHONE, sms);
  //}
}

bool sim900IsOn() {
  sim900.println("AT");
  unsigned long t = millis();
  while (millis() - t < 1000) {
    if (sim900.find("OK")) return true;
  }
  return false;
}

void ensureSim900On() {
  if (sim900IsOn()) {
    Serial.println("SIM900 deja allumé");
    return;
  }

  Serial.println("SIM900 eteint → allumage...");
  digitalWrite(SIM900_PWRKEY, HIGH);
  delay(1500);
  digitalWrite(SIM900_PWRKEY, LOW);

  delay(5000);  // boot GSM

  if (sim900IsOn()) {
    Serial.println("SIM900 allumé avec succes");
  } else {
    Serial.println("Echec allumage SIM900");
  }
}


// ===== CONTRÔLE VÉRIN =====
void extendActuator() {
  Serial.println(">>> EXTENSION VERIN <<<");
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 255);  // Vitesse max
  actuatorExtending = true;
  actuatorRetracting = false;
  actuatorStartTime = millis();
}

void retractActuator() {
  Serial.println(">>> RETRACTION VERIN <<<");
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  analogWrite(MOTOR_ENA, 255);  // Vitesse max
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

void gsmHardReset() {
  digitalWrite(GSM_RESET_PIN, LOW);
  delay(2000);  // ≥ 1 s (SIMCom spec)
  digitalWrite(GSM_RESET_PIN, HIGH);
  delay(10000);  // boot SIM900
}

bool gsmAlive() {
  sim900.flush();
  sim900.println("AT");

  unsigned long t0 = millis();
  while (millis() - t0 < GSM_TIMEOUT) {
    if (sim900.find("OK")) {
      return true;
    }
  }
  return false;
}

void printUptime() {
  unsigned long totalSeconds = millis() / 1000;

  unsigned long days = totalSeconds / 86400;
  totalSeconds %= 86400;

  unsigned long hours = totalSeconds / 3600;
  totalSeconds %= 3600;

  unsigned long minutes = totalSeconds / 60;
  unsigned long seconds = totalSeconds % 60;

  Serial.print("Uptime: ");
  Serial.print(days);
  Serial.print("j ");
  Serial.print(hours);
  Serial.print("h ");
  Serial.print(minutes);
  Serial.print("m ");
  Serial.print(seconds);
  Serial.println("s");
}

// void getUptimeString(char *buffer, size_t size) {
//   unsigned long totalSeconds = millis() / 1000;

//   unsigned long days = totalSeconds / 86400;
//   totalSeconds %= 86400;
//   unsigned long hours = totalSeconds / 3600;
//   totalSeconds %= 3600;
//   unsigned long minutes = totalSeconds / 60;
//   unsigned long seconds = totalSeconds % 60;

//   snprintf(
//     buffer,
//     size,
//     "%lu j %02lu h %02lu m %02lu s",
//     days, hours, minutes, seconds);
// }

void getUptimeString(char *buffer, size_t size) {
  static unsigned long lastMillis = 0;
  static unsigned long long totalSeconds64 = 0;

  unsigned long currentMillis = millis();
  unsigned long delta = currentMillis - lastMillis; // OK même après rollover
  lastMillis = currentMillis;

  totalSeconds64 += delta / 1000;

  unsigned long long total = totalSeconds64;

  unsigned long days    = total / 86400;
  total                %= 86400;
  unsigned long hours   = total / 3600;
  total                %= 3600;
  unsigned long minutes = total / 60;
  unsigned long seconds = total % 60;

  snprintf(
    buffer,
    size,
    "%lu j %02lu h %02lu m %02lu s",
    days, hours, minutes, seconds
  );
}

void sendUptimeSMS() {
  char uptimeMsg[64];
  getUptimeString(uptimeMsg, sizeof(uptimeMsg));
  envoyerSMS(MY_PHONE, "Systeme actif depuis : " + String(uptimeMsg));
}