/*
 * Détecteur de perte de courant avec Arduino UNO R4 WiFi et SIM900A
 * Envoie un SMS lors d'une coupure de courant
 * Utilise les pins 10 et 11 pour éviter les conflits avec le port série USB
 */

#include <SoftwareSerial.h>

// Configuration
#define POWER_SENSE_PIN 14      // Pin de détection de courant (A0 = pin 14 sur R4)
#define POWER_THRESHOLD 100     // Seuil de détection (ajuster selon votre montage)
#define CHECK_INTERVAL 2000     // Intervalle de vérification (ms)
#define DEBOUNCE_TIME 5000      // Temps anti-rebond (ms)

// Pins pour communication avec SIM900A
#define SIM_RX 10               // Pin 10 Arduino → TX du SIM900A
#define SIM_TX 11               // Pin 11 Arduino → RX du SIM900A

// Numéro de téléphone (format international: +33...)
const String PHONE_NUMBER = "+33612345678"; // À MODIFIER

// Variables globales
bool powerStatus = true;        // État du courant (true = présent)
bool smsSent = false;           // SMS déjà envoyé
unsigned long lastCheck = 0;
unsigned long powerLostTime = 0;

// Communication série logicielle pour SIM900A
SoftwareSerial sim900(SIM_RX, SIM_TX);

void setup() {
  // Serial pour debug USB
  Serial.begin(9600);
  
  // Communication avec SIM900A
  sim900.begin(9600);
  
  pinMode(POWER_SENSE_PIN, INPUT);
  
  Serial.println("Initialisation du detecteur de perte de courant...");
  delay(5000); // Attendre que le SIM900A démarre
  
  // Initialiser le module GSM
  initGSM();
  
  Serial.println("Systeme pret !");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Vérifier le courant à intervalles réguliers
  if (currentTime - lastCheck >= CHECK_INTERVAL) {
    lastCheck = currentTime;
    checkPowerStatus();
  }
  
  // Afficher les réponses du SIM900A dans le moniteur série
  if (sim900.available()) {
    Serial.write(sim900.read());
  }
}

void checkPowerStatus() {
  int powerValue = analogRead(POWER_SENSE_PIN);
  
  Serial.print("Valeur capteur: ");
  Serial.println(powerValue);
  
  // Détecter la perte de courant
  if (powerValue < POWER_THRESHOLD && powerStatus) {
    // Courant vient d'être perdu
    powerLostTime = millis();
    powerStatus = false;
    Serial.println("*** PERTE DE COURANT DETECTEE ! ***");
  }
  
  // Si le courant est perdu depuis assez longtemps et pas encore notifié
  if (!powerStatus && !smsSent && (millis() - powerLostTime > DEBOUNCE_TIME)) {
    sendPowerOutageAlert();
    smsSent = true;
  }
  
  // Détecter le retour du courant
  if (powerValue >= POWER_THRESHOLD && !powerStatus) {
    powerStatus = true;
    Serial.println("*** COURANT RETABLI ***");
    
    // Envoyer SMS de rétablissement (optionnel)
    if (smsSent) {
      sendPowerRestoredAlert();
      smsSent = false;
    }
  }
}

void initGSM() {
  Serial.println("Initialisation du module GSM...");
  
  // Test de communication
  sim900.println("AT");
  delay(1000);
  printGSMResponse();
  
  // Mode texte pour SMS
  sim900.println("AT+CMGF=1");
  delay(1000);
  printGSMResponse();
  
  // Configurer le jeu de caractères
  sim900.println("AT+CSCS=\"GSM\"");
  delay(1000);
  printGSMResponse();
  
  // Vérifier la qualité du signal
  sim900.println("AT+CSQ");
  delay(1000);
  printGSMResponse();
  
  // Vérifier l'enregistrement réseau
  sim900.println("AT+CREG?");
  delay(1000);
  printGSMResponse();
  
  Serial.println("Module GSM initialise");
}

void sendPowerOutageAlert() {
  Serial.println("Envoi du SMS d'alerte...");
  
  // Définir le numéro de destination
  sim900.print("AT+CMGS=\"");
  sim900.print(PHONE_NUMBER);
  sim900.println("\"");
  delay(1000);
  
  // Message
  sim900.print("ALERTE: Perte de courant detectee ! ");
  sim900.print("Heure: ");
  sim900.print(millis() / 1000);
  sim900.println(" secondes depuis demarrage");
  delay(500);
  
  // Envoyer Ctrl+Z pour terminer le SMS
  sim900.write(26);
  delay(5000);
  
  printGSMResponse();
  Serial.println("SMS d'alerte envoye !");
}

void sendPowerRestoredAlert() {
  Serial.println("Envoi du SMS de retablissement...");
  
  sim900.print("AT+CMGS=\"");
  sim900.print(PHONE_NUMBER);
  sim900.println("\"");
  delay(1000);
  
  sim900.print("INFO: Courant retabli. ");
  sim900.print("Duree de coupure: ");
  sim900.print((millis() - powerLostTime) / 1000);
  sim900.println(" secondes");
  delay(500);
  
  sim900.write(26);
  delay(5000);
  
  printGSMResponse();
  Serial.println("SMS de retablissement envoye !");
}

void printGSMResponse() {
  delay(500);
  while (sim900.available()) {
    Serial.write(sim900.read());
  }
}

