/*
 * Test SMS MINIMAL avec SIM900A
 *
 * Fonctionnement :
 * 1. Envoyer un SMS avec le texte "REARMER" au module
 * 2. Le module répond automatiquement "Bonjour, j'ai recu votre message !"
 *
 * Connexions :
 * - SIM900A TX  -> Arduino Pin 2 (RX)
 * - SIM900A RX  -> Arduino Pin 3 (TX)
 * - SIM900A GND -> Arduino GND
 * - SIM900A VCC -> Alimentation externe 5V/2A
 */

#include <SoftwareSerial.h>

// Pins pour SIM900A
#define SIM_RX 0
#define SIM_TX 1
#define SIM_POWER 4
#define POWER_SENSE_PIN 14      // Pin A0 - Capteur SCT-013-030

#define POWER_THRESHOLD 50      // Seuil détection perte courant (amplitude AC)
#define DEBOUNCE_TIME 5000      // Anti-rebond détection perte (ms)


#define VOLTAGE_REF 5.0         // Tension de référence Arduino
#define ADC_COUNTS 1024.0       // Résolution ADC
#define SCT_RATIO 30.0          // Ratio du capteur (30A pour 1V)
#define VOLTAGE_MAINS 230.0     // Tension secteur (230V en France)
#define CHECK_INTERVAL 2000     // Intervalle vérification courant (ms)


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





// ===== COMMANDES SMS =====
const String CMD_REARM = "REARMER";
const String CMD_STATUS = "STATUS";
const String CMD_TEST = "TEST";
const String CMD_CONSO = "CONSO";

bool started = false;

// Votre numéro de téléphone (format international)
const String MY_PHONE = "+33672957874"; // CHANGEZ CECI !

// Communication série
SoftwareSerial sim900(SIM_RX, SIM_TX);

String smsBuffer = "";

void setup() {
  // Moniteur série pour debug
  Serial.begin(9600);

  pinMode(POWER_SENSE_PIN, INPUT);
 
  Serial.println("=================================");
  Serial.println("   TEST SMS MINIMAL SIM900A      ");
  Serial.println("=================================");
 
  delay(3000); // Attendre démarrage SIM900A
 
  // Tester différents baud rates
  int baudRates[] = {9600, 19200, 115200, 4800};
 
  for (int i = 0; i < 4; i++) {
    Serial.print("Test baud rate: ");
    Serial.println(baudRates[i]);
   
    sim900.begin(baudRates[i]);
    delay(500);
   
    // Test AT
    sim900.println("AT");
    delay(1000);
   
    if (sim900.available()) {
      Serial.print("REPONSE a ");
      Serial.print(baudRates[i]);
      Serial.println(" baud:");
      while (sim900.available()) {
        Serial.write(sim900.read());
      }
      Serial.println("\n=== BAUD RATE TROUVE ! ===");
      started = true;
      break;
    } else {
      Serial.println("Pas de reponse");
      started=false;
    }
  }
 
 if (started){
            pinMode(4,OUTPUT);
            digitalWrite(4,HIGH);
            delay(2000);
            digitalWrite(4,LOW);
            Serial.println("<=================================");
            Serial.println("Initialisation module GSM...");
          
            // Communication avec SIM900A (utiliser le bon baud rate trouvé)
            sim900.begin(9600); // Ajustez selon le résultat ci-dessus
          
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
          
            Serial.println("=================================");
            Serial.println("Module GSM pret !");
            Serial.println("Envoyez un SMS avec une commande reconnue");
            Serial.println("=================================");
 }
}

void loop() {

    unsigned long currentTime = millis();
  
  // Vérifier le courant à intervalles réguliers
  if (currentTime - lastCheck >= CHECK_INTERVAL) {
    lastCheck = currentTime;
    checkPowerStatus();
  }

  // Lire données du SIM900A
  while (sim900.available()) {
    char c = sim900.read();
    Serial.write(c); // Afficher dans le moniteur série
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
  message.toUpperCase(); // Convertir en majuscules
 
  Serial.print("Expediteur: ");
  Serial.println(numero);
  Serial.print("Message: ");
  Serial.println(message);
 
  // Répondre si le message contient "REARMER"
  if (message.indexOf(CMD_REARM) >= 0) {
    Serial.println("Commande " + CMD_REARM + " detectee !");
    envoyerSMS(numero, "Bonjour ! J'ai recu votre message " + CMD_REARM + " :)");
  }
  else if (message.indexOf("TEST") >= 0) {
    Serial.println("Commande TEST detectee !");
    envoyerSMS(numero, "Test OK ! Le module fonctionne correctement.");
  }
  else if (message.indexOf("STATUS") >= 0) {
    Serial.println("Commande STATUS detectee !");
    envoyerSMS(numero, "Systeme actif. Temps: " + String(millis()/1000) + "s");
  }
  else if (message.indexOf("CONSO") >= 0) {
    Serial.println("Commande CONSO detectee !");
    //envoyerSMS(numero, "Consommation actuelle: "+currentPower+"W / "+currentAmps+"A");
    //String currentPowerDetected = checkPowerStatus();
    //Serial.println(currentPowerDetected);
    handleConsoCommand();
  }
  else {
    Serial.println("Message non reconnu");
    envoyerSMS(numero, "Commande '" + message + "' non reconnue.\nCommandes disponibles:\n'" + CMD_REARM + "', '" + CMD_STATUS + "', '" + CMD_TEST + "', '" + CMD_CONSO + "'");
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
  

  if (amplitude < POWER_THRESHOLD){
  lastPowerBeforeCut = powerWatts;
  lastCurrentBeforeCut = currentRMS;
  }

  // Détection perte de courant
  if (amplitude < POWER_THRESHOLD && powerStatus) {
    powerLostTime = millis();
    powerStatus = false;
    Serial.println("*** PERTE DE COURANT DETECTEE ! ***");
  }
  
  // Envoi SMS après anti-rebond
  if (!powerStatus && !smsSent && (millis() - powerLostTime > DEBOUNCE_TIME)) {
    //sendPowerOutageAlert();
    char bufPower[10];
    char bufAmps[10];
    char sms[80];

    dtostrf(lastPowerBeforeCut, 6, 0, bufPower);   // ex "  742"
    dtostrf(lastCurrentBeforeCut, 5, 2, bufAmps);    // ex " 3.24"

    sprintf(sms,
      "/!\\ PERTE DE COURANT!\nConsommation avant coupure:\n     • %s W\n     • %s A",
      bufPower,
      bufAmps
    );

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
  char bufPower[10];
  char bufAmps[10];
  char sms[80];

  dtostrf(powerWatts, 6, 0, bufPower);
  dtostrf(currentRMS, 5, 2, bufAmps);

  sprintf(sms,
    "Consommation actuelle:\n     • %s W\n     • %s A",
    bufPower,
    bufAmps
  );

  envoyerSMS(MY_PHONE, sms);
}
