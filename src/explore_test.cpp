#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <String>
#include <iostream>
#include <memory>

#include <Arduino.h>
#include <QTRSensors.h>
#include <SPI.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <MFRC522.h>


using namespace std;

//-------------------- FIREBASE -----------------------
#define WIFI_SSID "eduroam31"
#define WIFI_PASSWORD "sekiztane1"
#define API_KEY "AIzaSyD3BE-hRNRFyzfK1d98scXM5zG5w5iX3fw"
#define DATABASE_URL "https://ladsceng483-default-rtdb.europe-west1.firebasedatabase.app/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
// Path to the field you want to monitor
String activeTaskPath ="/active_task";
String deliveryLocation = "/active_task/delivery_location";
String pickupLocation = "/active_task/pick_up_location";

unsigned long sendDataPrevMillis=0;
bool signupOK = false;
// ------------------- RFID PINS ----------------------
#define SS_PIN  5   // ESP32 pin for MFRC522 SS/SDA
#define RST_PIN 25  // ESP32 pin for MFRC522 RST
MFRC522 rfid(SS_PIN, RST_PIN);  // Create MFRC522 instance

// ------------------- QTR SENSOR SETUP ----------------
QTRSensors qtr;
const uint8_t SensorCount = 6;
uint16_t sensorValues[SensorCount];


// ------------------- PID PARAMETERS ------------------
float Kp = 0.1;
float Ki = 0.0;
float Kd = 0.0;

int lastError = 0;
float integral = 0;

// ------------------- MOTOR PINS ----------------------
int motor1Pin1 = 15;
int motor1Pin2 = 2;
int enable1Pin = 17;

int motor2Pin1 = 0;
int motor2Pin2 = 4;
int enable2Pin = 22;

// ------------------- MOTOR PWM PROPERTIES ------------
const int freq = 30000;
const int pwmChannel1 = 0;
const int pwmChannel2 = 1;
const int resolution = 8;
const int maxSpeed = 220;

// ------------------- ULTRASONIC PINS -----------------
const int trigPin = 26;
const int echoPin = 21;

#define SOUND_SPEED 0.034

// ------------------- MAPPING STRUCTURES --------------
//every left turn increments direction by 1, every right turn decrements it by 1. U turn does so by 2. then take mod4
int direction=0; //0(up) 1(left) 2(down) 3(right)
int prevDirection=0;

// Each Node can be an intersection or an RFID tag
struct Node {
  String uid;         // e.g. "RFID_XXYY" or "INT_1", "INT_2"
  bool isRFID;
  // adjacency: which other nodes connect to this node?
  vector<shared_ptr<Node>> ptrs={nullptr,nullptr,nullptr,nullptr};
  std::vector<unsigned long> costs={0,0,0,0};  // travel time in ms
  
};

//std::vector<Node> graph;            // Our global graph
std::vector<shared_ptr<Node>> graph;
vector<tuple<String,shared_ptr<Node>>> cmd_stack;
shared_ptr<Node> current_Node_Ptr;

std::shared_ptr<Node> wall = std::make_shared<Node>();

vector<shared_ptr<Node>> targetpath;

vector<String> orders;

/*0: Waiting on Start
	1: Exploration
	2: Going to a node
	3: Waiting for proceeding (app notified)
	4: Going back to start node */
int mode = 0;  

bool backtrack = false;

int currentNodeIndex = -1;              // Index of the current node in the graph
int startNodeIndex = -1;                // Index of the start node in the graph
int intersectionCounter = 0;            // Generate unique intersection IDs
unsigned long segmentStartTime = 0;     // measure travel time between nodes
unsigned long ignoreRFIDUntil = 0;      // ignore RFID reads until this time
unsigned long ignoreIntersectionUntil = 0;
String last_read_tagUID = "";

String startNodeName = "";
// ------------------- FUNCTION DECLARATIONS -----------
int  findNodeIndex(const String& uid); 

int  addNodePtr(const String uid, bool isRFID, int mask);


bool checkForRFID();

bool obstacleDetection();
bool handleIntersectionIfNeeded();


void driveMotors(int leftSpeed, int rightSpeed); 
void handleLineFollow(); 

String getFinalGraphString();
void updateTaskNodes();

void rstVars();
void turnL();
void turnR();
void merge_two_nodes(shared_ptr<Node> trueNode,shared_ptr<Node> oldNode);
void link_via_direction(shared_ptr<Node> n, unsigned long cost);
void process_cmd(String cmd);
void printFinalGraphState();
void mode_set(int a);

void updateGraphInDatabase(String graph);
void streamCallback_order(FirebaseStream data);
void streamCallback_wait(FirebaseStream data);
void streamTimeoutCallback(bool timeout);
void updateTaskNodes();
String detectIntersection();
String createIntersectionID(String type, bool rfid); 
String getFinalGraphString();

vector<shared_ptr<Node>> djikstra(String startName, String endName);

tuple<String, bool, bool> interpret_triple(tuple<String, String, String> triple);

//----mode functions:

void ExplorationPhase();
void readyForOrder(); 
void goToNode();
void waitForPickUp();
void updateGraphInDatabase(String graph);
void mode_set(int mode);

// --------------- SETUP -------------------------------
void setup()
{
  Serial.begin(9600);
  Serial.println("Initializing...");

  // ----- WIFI CONNECT -----
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("connecting to wifi");
  while(WiFi.status() != WL_CONNECTED){
      Serial.print(".");delay(300);
  }
  Serial.println();
  Serial.print("connected IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  // ----- FIREBASE SET UP -----
  config.api_key=API_KEY;
  config.database_url=DATABASE_URL;
  if(Firebase.signUp(&config,&auth,"","")){
      Serial.println("signup OK");
      signupOK=true;
  } else {
      Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  //config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.RTDB.beginStream(&fbdo, activeTaskPath)) {
    Serial.println("Firebase stream started successfully.");
    Firebase.RTDB.setStreamCallback(&fbdo, streamCallback_order, streamTimeoutCallback);
  } else {
    Serial.printf("Could not start stream, %s\n", fbdo.errorReason().c_str());
  }
  if (Firebase.RTDB.beginStream(&fbdo, "/Robot/waiting")) {
    Serial.println("Firebase stream started successfully.");
    Firebase.RTDB.setStreamCallback(&fbdo, streamCallback_wait, streamTimeoutCallback);
  } else {
    Serial.printf("Could not start stream, %s\n", fbdo.errorReason().c_str());
  }

  // ----- MOTOR SETUP -----
  pinMode(motor1Pin1, OUTPUT);
  pinMode(motor1Pin2, OUTPUT);
  pinMode(motor2Pin1, OUTPUT);
  pinMode(motor2Pin2, OUTPUT);

  ledcAttachPin(enable1Pin, pwmChannel1);
  ledcSetup(pwmChannel1, freq, resolution);

  ledcAttachPin(enable2Pin, pwmChannel2);
  ledcSetup(pwmChannel2, freq, resolution);

  // ----- QTR SENSOR SETUP -----
  qtr.setTypeAnalog();
  qtr.setSensorPins((const uint8_t[]){36,39,34,35,32,33}, SensorCount);
  qtr.setEmitterPin(2);

  // ----- CALIBRATION -----
  Serial.println("Calibrating sensors. Move the robot over the line...");
  for (uint16_t i = 0; i < 200; i++)
  {
    qtr.calibrate();
    if (i % 50 == 0) {
      Serial.print("Calibration progress: ");
      Serial.print((i / 200.0) * 100);
      Serial.println("%");
    }
  }
  Serial.println("Calibration complete!");

  // (Optional) Print calibration values
  Serial.println("Calibration minimum values:");
  for (uint8_t i = 0; i < SensorCount; i++) {
    Serial.print(qtr.calibrationOn.minimum[i]);
    Serial.print(' ');
  }
  Serial.println();

  Serial.println("Calibration maximum values:");
  for (uint8_t i = 0; i < SensorCount; i++) {
    Serial.print(qtr.calibrationOn.maximum[i]);
    Serial.print(' ');
  }
  Serial.println();

  // ----- ULTRASONIC SETUP -----
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  Serial.println("Ultrasonic ready...");

  // ----- RFID READER SETUP -----
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("Tap an RFID/NFC tag on the RFID-RC522 reader");

  delay(1000);

  segmentStartTime = millis(); // start measuring travel time from the initial node
  ignoreIntersectionUntil=millis();
  mode_set(1);
  cout<<"starting..."<<endl;
}
// --------------- MAIN LOOP ---------------------------
void loop()
{
  switch(mode)
  {
    case 0:
      readyForOrder();
    break;

    case 1:
      ExplorationPhase();
    break;

    case 2:
      goToNode();//go to deliver(targetpath given either by readyForOrder or streamCallback_wait)
    break;

    case 3:
      waitForPickUp();
    break;

    case 4:
      goToNode();//go back to start(targetpath given by streamCallback_wait)
  }
  
}


// -----------------------------------------------------
//                       MODES
// -----------------------------------------------------

void reset_robot(){
    backtrack=false;

    currentNodeIndex = -1;              // Index of the current node in the graph
    startNodeIndex = -1;                // Index of the start node in the graph
    intersectionCounter = 0;            // Generate unique intersection IDs
    segmentStartTime = millis();     // measure travel time between nodes
    ignoreRFIDUntil = millis();      // ignore RFID reads until this time
    ignoreIntersectionUntil = millis();
    last_read_tagUID = "";

    startNodeName="";
    for (auto& node : graph) {
      node->ptrs.clear(); 
    }
    graph.clear();
    current_Node_Ptr=nullptr;

    cmd_stack.clear();
    int direction=0; //0(up) 1(left) 2(down) 3(right)
    int prevDirection=0;
    targetpath.clear();
    orders.clear();

}
void mode_set(int a){
  FirebaseJson json;

  if(a==3){
  json.set("waiting", true);
  if (Firebase.RTDB.set(&fbdo, "/robot/waiting", &json)) {
    Serial.println("Waiting updated in Firebase.");
  } else {
    Serial.printf("Failed to update waiting: %s\n", fbdo.errorReason().c_str());
  }
  }else if(a==1){
    reset_robot();
  } else if(mode==1 && a==0){
    updateGraphInDatabase(getFinalGraphString());
  }

  mode=a;
  //update firebase
  
  json.set("current_mode", mode);  // Set the 'current_mode' field to the new mode value
  
  // Write the updated mode to the Firebase Realtime Database under the 'robot' node
  if (Firebase.RTDB.set(&fbdo, "/robot/current_mode", &json)) {
    Serial.println("Mode updated in Firebase.");
  } else {
    Serial.printf("Failed to update mode: %s\n", fbdo.errorReason().c_str());
  }

}
void ExplorationPhase(){
  handleLineFollow();

  if(millis() > ignoreIntersectionUntil ){
        handleIntersectionIfNeeded();
        printFinalGraphState();
    }
  if(millis()> ignoreRFIDUntil){
        if(checkForRFID()){
          process_cmd("P");
          printFinalGraphState();
        };
    }
    
}

void waitForPickUp(){
  //do nothing. streamCallback_wait() will change mode.
  //if necessery, add timer here.
}

void readyForOrder(){
  //checks if the orders array has been given value by streamCallback_order()
  if(orders.size()>0){
  String endName=orders[0];

  targetpath=djikstra(current_Node_Ptr->uid,endName);
  updateTaskNodes();
  mode_set(2);
  orders.erase(orders.begin());
  return;
  } else {
    return;
  }
}

void goToNode(){

    handleLineFollow();
      if(checkForRFID() && detectIntersection()!="I"){
        cout<<"I was at: "<< current_Node_Ptr->uid<<endl;
        cout<<"is it "<< targetpath[0]->uid <<endl;
        if(current_Node_Ptr==targetpath[0]){
          cout<<"correct, deleted path begin."<<endl;
          targetpath.erase(targetpath.begin());
        }else{
          cerr<<"[ERROR] at an unexpected location"<<endl;
          cerr<<"mismatched target path and graph"<<endl;
          cerr<<"==============="<<endl;
          cerr<<"current node: " << current_Node_Ptr->uid<<endl;
          cerr<<"current node as per targetpath: "<< targetpath[0]->uid<<endl;
          cerr<<"================="<<endl;
          cerr<<"[BREAKING LOOP]"<<endl; 
          mode=-99;
          return;}
        if(targetpath.empty()){
          cout<<"end of the path is here"<<endl;
          cout<<"[BREAKING LOOP]"<<endl; 
          rstVars();
          if(mode==2){// was going for task
            mode_set(3);
          } else {// was going back to start node
            mode_set(0);
          }
          return;
        }
        for(int i=0; i< current_Node_Ptr->ptrs.size();i++){
          if(current_Node_Ptr->ptrs[i]==targetpath[0]){
            //int decision=direction-i;
            cout<<"switch-case: " << (direction-i)<<endl;
            switch (direction-i)
            {
              case -1:
              case 3:
                cout<<"turned Left"<<endl;
                turnL();
              break;
              case 2:
              case -2:
                cout<<"turned backwards"<<endl;
                turnL();
                turnL();
              break;
              case 1: 
              case -3:
                cout<<"turned right"<<endl;
                turnR();
              break;
              default:
                cout<<"went straight"<<endl;
              break;
            }

            current_Node_Ptr=current_Node_Ptr->ptrs[direction];
            cout<<"I am now at: "<< current_Node_Ptr->uid<<endl;
            i+=graph[currentNodeIndex]->ptrs.size();//break from for loop
            
            return;
          }
        }

        cerr<<"[ERROR] next node on target path is not current node's neighbor."<<endl;
        cerr<<"==============="<<endl;
        cerr<<"current node: " << current_Node_Ptr->uid<<endl;
        cerr<<"neighbors:"<<endl;

        for (int i = 0; i < 4; i++) {
          cerr<<"   ";
          if (current_Node_Ptr->ptrs[i]!=nullptr && current_Node_Ptr->ptrs[i]!=wall) {
            cerr << (i == 0 ? "0-N: " : i == 1 ? "1-E: " : i == 2 ? "2-S: " : "3-W: ");
            cerr << current_Node_Ptr->ptrs[i]->uid <<endl;
          } else {
            cerr << (i == 0 ? "0-N: None" : i == 1 ? "1-E: None" : i == 2 ? "2-S: None" : "3-W: None")<<endl;
          }
        }

        cerr<<"----------------"<<endl;
        cerr<<"next node as per targetpath: "<< targetpath[0]->uid<<endl;
        cerr<<"================="<<endl;
        cerr<<"[BREAKING LOOP]"<<endl; 
        mode=-99;
        return;
    }
} 



// -----------------------------------------------------
//                 HELPER FUNCTIONS
// -----------------------------------------------------

void streamCallback_order(FirebaseStream data) {
  Serial.println("Data changed!");

  // Check if the data is JSON
  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_json) {
    FirebaseJson jsonData = data.jsonObject(); // Parse JSON object
    
    FirebaseJsonData result; // Create a FirebaseJsonData object to store the result

if (jsonData.get(result, "pick_up_location") && result.typeNum == FirebaseJson::JSON_STRING) {
      String pickUpLocation = result.stringValue; // Get the pick-up location value as a string
      orders.push_back(pickUpLocation.c_str());
    }

    if (jsonData.get(result, "delivery_location") && result.typeNum == FirebaseJson::JSON_STRING) {
      String deliveryLocation = result.stringValue; // Get the delivery location value as a string
      orders.push_back(deliveryLocation.c_str());
    }

    //orders.clear();
    // Print the updated orders vector
    Serial.println("Updated Orders:");
    for (const auto& order : orders) {
      Serial.println(order.c_str());
    }

  } else {
    Serial.println("Unexpected data type received.");
  }
}


void streamCallback_wait(FirebaseStream data) {
   Serial.println("Stream callback triggered for 'waiting'!");

  // Check if the data type is boolean
  if (data.dataTypeEnum() == fb_esp_rtdb_data_type_boolean) {
    bool waitingValue = data.boolData(); // Extract the boolean value

    if (!waitingValue) { // Check if 'waiting' has become false
      
      if(mode==3){
        if(orders.size()>0){
          targetpath=djikstra(current_Node_Ptr->uid,orders[0]);
          orders.erase(orders.begin());
          mode_set(1);
        } else{
          targetpath=djikstra(current_Node_Ptr->uid, startNodeName);
          mode_set(4);
        }
        updateTaskNodes();
      }else{return;}
    }
  } else {
    Serial.println("Unexpected data type received for 'waiting'.");
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, reconnecting...");
  } else {
    Serial.println("Stream error.");
  }
}
/**
 * @brief set ignore vars to 0
 */
void rstVars(){
  ignoreIntersectionUntil=0;
  ignoreRFIDUntil=0;
}



/**
 * @brief Find node index in global 'graph' by its UID
 */
int findNodeIndex(const String& uid) {
  for (size_t i = 0; i < graph.size(); i++) {
    if (graph[i]->uid == uid) {
      return i;
    }
  }
  return -1; // not found
}

// /**
//  * @brief Add a new node to the graph
//  */
int addNodePtr(const String uid, bool isRFID, int mask){

  auto n = std::make_shared<Node>();
 
  n->uid=uid;

  n->isRFID = isRFID;

  if(!(mask&(1<<0))){
    n->ptrs[0]=wall;
  }

  if(!(mask&(1<<1))){
    n->ptrs[1]=wall;
  }

  if(!(mask&(1<<2))){
    n->ptrs[2]=wall;
  }

  if(!(mask&(1<<3))){
    n->ptrs[3]=wall;
  }

  graph.push_back(n);

  return graph.size() - 1;
}
/**
 * @brief MUST be called before updating current_node_ptr.
 * take in the new node, create a connection between it and the current_node_ptr
 */
void link_via_direction(shared_ptr<Node> new_node, unsigned long cost){

  if(current_Node_Ptr==nullptr){

    return;
  }

  current_Node_Ptr->ptrs[prevDirection]=new_node;

  current_Node_Ptr->costs[prevDirection]=cost;

  int opp= (prevDirection+2)%4;

  new_node->ptrs[opp]=current_Node_Ptr;

  new_node->costs[opp]=cost;


}

bool obstacleDetection(){
digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  // Sets the trigPin on HIGH state for 10 micro seconds
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Reads the echoPin, returns the sound wave travel time in microseconds
  long duration = pulseIn(echoPin, HIGH);
  
  // Calculate the distance
  float distanceCm = duration * SOUND_SPEED/2;

return distanceCm<10.0;
}

/**
 * @brief Basic motor driver
 */
void driveMotors(int leftSpeed, int rightSpeed)
{

  if(obstacleDetection()){
    ledcWrite(pwmChannel1, 0);
    ledcWrite(pwmChannel2, 0);
    return;
  }

  // Left motor
  if (leftSpeed >= 0) {
    digitalWrite(motor1Pin1, HIGH);
    digitalWrite(motor1Pin2, LOW);
  } else {
    digitalWrite(motor1Pin1, LOW);
    digitalWrite(motor1Pin2, HIGH);
  }
  ledcWrite(pwmChannel1, abs(leftSpeed));

  // Right motor
  if (rightSpeed >= 0) {
    digitalWrite(motor2Pin1, HIGH);
    digitalWrite(motor2Pin2, LOW);
  } else {
    digitalWrite(motor2Pin1, LOW);
    digitalWrite(motor2Pin2, HIGH);
  }
  ledcWrite(pwmChannel2, abs(rightSpeed));
}

/**
 * @brief Reads QTR sensors, does PID line follow
 */
void handleLineFollow()
{
  
  uint16_t position = qtr.readLineBlack(sensorValues);
  int error = position - 2500;  // for 6 sensors
  float proportional = error;
  integral += error;
  float derivative = error - lastError;

  float correction = (Kp * proportional) + (Ki * integral) + (Kd * derivative);
  correction = constrain(correction, -maxSpeed, maxSpeed);

  // We'll do a simple approach: a "base speed" approach, or direct approach.
  int minSpeed = 170;
  int leftMotorSpeed  = maxSpeed + correction;
  int rightMotorSpeed = maxSpeed - correction;

  leftMotorSpeed  = constrain(leftMotorSpeed, 0, maxSpeed);
  rightMotorSpeed = constrain(rightMotorSpeed, 0, maxSpeed);

  // scale them so they never go below minSpeed
  float rangeFactor = (float)(maxSpeed - minSpeed) / (float)maxSpeed; 
  leftMotorSpeed  = minSpeed + rangeFactor * leftMotorSpeed;
  rightMotorSpeed = minSpeed + rangeFactor * rightMotorSpeed;

  driveMotors(leftMotorSpeed, rightMotorSpeed);
  lastError = error;
}

/**
 * @brief Check if a new RFID card is present, update ignoreRFIDuntil
 */
bool checkForRFID()
{
  if (rfid.PICC_IsNewCardPresent() || ignoreRFIDUntil < millis()) {
    ignoreRFIDUntil = millis() + 2000;
    if (rfid.PICC_ReadCardSerial()) {
      // Build the UID String
      last_read_tagUID = "";
      for (int i = 0; i < rfid.uid.size; i++) {
        last_read_tagUID += String(rfid.uid.uidByte[i], HEX);
      }
      last_read_tagUID.toUpperCase();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      
      return true;
    }
  }
  return false;
}

/**
 * @brief returns a T, L, R, or U based on intersection.
 * Returns I if no intersection.
 * Updates ignoreIntersectionUntil
 */
String detectIntersection() {

if(millis() < ignoreIntersectionUntil ){
        return "I";
    }
  ignoreIntersectionUntil=millis()+2000;
  qtr.readCalibrated(sensorValues);
  int threshold =400; // General threshold adjustment

  bool leftSide = (sensorValues[0] > threshold &&
                   sensorValues[1] > threshold &&
                   sensorValues[2] > threshold);
  bool rightSide = (sensorValues[3] > threshold &&
                    sensorValues[4] > threshold &&
                    sensorValues[5] > threshold);

  bool nothing = (sensorValues[0] < threshold &&
                  sensorValues[1] < threshold &&
                  sensorValues[2] < threshold &&
                  sensorValues[3] < threshold &&
                  sensorValues[4] < threshold &&
                  sensorValues[5] < threshold);

  // Determine intersection type
  if (leftSide && rightSide) {
    return "T";  // T or + intersection
  } else if (leftSide) {
    return "L";  // Left intersection
  } else if (rightSide) {
    return "R";  // Right intersection
  } else if(nothing){
    return "U";
  } else {
    return "I";  // Unknown or straight
  }

}

/**
 * @brief Create an ID String, e.g. "INT_0_T", "INT_1_L"
 */
String createIntersectionID(String type, bool rfid)
{ 
  if(rfid){
    return last_read_tagUID;
  }else{
  String id = "INT_";
  id += String(intersectionCounter++);
  id += "_";
  // append the type code
  id += type; 
  return id;
  }
}

/**
 * @brief If intersection is detected process_cmd().
 *  Only for exploration phase
 */
bool handleIntersectionIfNeeded() {
    
    // Determine intersection type
    String type = detectIntersection();
    if(type=="I"){return false;}
    process_cmd(type);

    return true;
}

void turnL(){direction++;direction=(direction+4)%4;
      driveMotors(-220, 220);  // Pivot left
      delay(500);
      }
void turnR(){direction--;direction=(direction+4)%4;
      driveMotors(220, -220);  // Pivot left
      delay(500);
      }

/**
 * @brief Add a new node or do backtracking. S for startNode.
 */
void process_cmd(String cmd){
    if(current_Node_Ptr==nullptr){
      if(cmd=="P")
      {
        cmd="S";
        String UID = createIntersectionID(cmd, true);
        startNodeName=UID;
      } 
      else 
      { 
        return;
      }
    }
    else if(last_read_tagUID==startNodeName)
    {
        cmd="S";
    }

    if(cmd=="S"){
      if(current_Node_Ptr==nullptr){
        cout<< "We are at the start." << endl;
        int mask=0;
        int mflag= (direction+2)%4;
        mask = mask | 1 << mflag;
        mask = mask | 1 << direction;
        int newNodeIndex=addNodePtr(startNodeName, false, mask);

        current_Node_Ptr=graph[newNodeIndex];

        //tuple <String, shared_ptr<Node>> tup=make_tuple(cmd,current_Node_Ptr);
        cmd_stack.push_back(make_tuple(cmd,current_Node_Ptr));
        return;
      } else {
        cout<< "Returned to the start; We are done with exploration mode." << endl;
        prevDirection=direction;
        unsigned long now = millis();
        unsigned long travelTime = now - segmentStartTime;
        segmentStartTime = millis();
        link_via_direction(graph[findNodeIndex(startNodeName)],travelTime);
        current_Node_Ptr=graph[findNodeIndex(startNodeName)];
        printFinalGraphState();
        driveMotors(-220, -220);  // back up before the start node
        delay(1000);
        mode_set(0);
        //do stuff
        return;
      }
    }

    bool isRFID=false;
    if(cmd=="P"){
      isRFID=true;
    }
    String UID = createIntersectionID(cmd, isRFID);
    prevDirection=direction;
    int newNodeIndex=-1;
    int mask=0;
    int mflag= (direction+2)%4;
    mask = mask | 1 << mflag;
    // Handle turn decisions
    driveMotors(0, 0);

  if(backtrack==false){
    if (cmd == "L") {
      // Left intersection
      cout<< "Turning LEFT (L intersection)..." << endl;
      mflag= (direction+1)%4;
      mask = mask | 1 << mflag;
      turnL();
    } else if (cmd == "R") {
      // Right intersection
      cout<< "Turning RIGHT (R intersection)..." << endl;
      mflag= (direction+3)%4;
      mask = mask | 1 << mflag;
      turnR();
    } else if (cmd == "T") {
      // T or + intersection
      cout<< "T intersection detected" << endl;
      mflag= (direction+1)%4;
      mask = mask | 1 << mflag;
      mflag= (direction+3)%4;
      mask = mask | 1 << mflag;
      cout<< "defaulting LEFT...";
      turnL(); 
    }  else if (cmd == "U") {
      // T or + intersection
      cout<< "U: deadend detected. reversing..." << endl;

      backtrack=true;

      turnL();
      turnL();
    } else if (cmd == "P") {
      cout<< "RFID tag detected, continuing forward" << endl;
    } else {
      // Unknown or straight
      cout<< "Unknown intersection. Going straight..." << endl;
    }

      newNodeIndex=addNodePtr(UID, isRFID, mask);
      unsigned long now = millis();
      unsigned long travelTime = now - segmentStartTime;
      segmentStartTime = millis();
      link_via_direction(graph[newNodeIndex], travelTime);

      current_Node_Ptr=graph[newNodeIndex];

      //make tie
      //tuple <String, shared_ptr<Node>> tup=make_tuple(cmd,current_Node_Ptr);
      cmd_stack.push_back(make_tuple(cmd,current_Node_Ptr));

  } else {//backtrack
    if(cmd_stack.size()<2){
      newNodeIndex=addNodePtr(UID, isRFID, mask);
      current_Node_Ptr=graph[newNodeIndex];
      //make tie
      //tuple <String, shared_ptr<Node>> tup=make_tuple(cmd,current_Node_Ptr);
        cmd_stack.push_back(make_tuple(cmd,current_Node_Ptr));
      cout<< "backtrace, not enough for a triple, pushed cmd" << endl;
      return;
    } else{
    tuple <String, shared_ptr<Node>> u_tuple=cmd_stack[cmd_stack.size()-1];
    if(get<0>(u_tuple)!="U"){
      newNodeIndex=addNodePtr(UID, isRFID, mask);
      current_Node_Ptr=graph[newNodeIndex];
      //make tie
      //tuple <String, shared_ptr<Node>> tup=make_tuple(cmd,current_Node_Ptr);
        cmd_stack.push_back(make_tuple(cmd,current_Node_Ptr));
      cout<< "backtrace, unexpected top, pushed cmd" << endl;
      return;
    }

    tuple <String, shared_ptr<Node>> y_tuple=cmd_stack[cmd_stack.size()-2];
    //tuple <String,bool,bool> triple_decision; //decision, error, remain_backtrace
    String decision;
    bool error;
    bool remain_backtrace;
    tie(decision,error,remain_backtrace)=interpret_triple(tie(get<0>(y_tuple),"U",cmd));
    if(error){
      cout<< "ERROR: map is corrupted for triple" << endl;
      return;
    }
    if (decision!=""){
      cout<< "backtrace decision:";
      cout<< decision<< endl;
      newNodeIndex=addNodePtr(UID, isRFID, mask);
      unsigned long now = millis();
      unsigned long travelTime = now - segmentStartTime;
      segmentStartTime = millis();
      link_via_direction(graph[newNodeIndex], travelTime);
      current_Node_Ptr=graph[newNodeIndex];
      merge_two_nodes(current_Node_Ptr, get<1>(y_tuple));
      //merge in backtrace(y_node,cmd)
      if(decision=="L"){
        turnL();
      } else if (decision=="R"){
        turnR();
      } 
      cout<< "new direction: ";
      cout<< direction << endl;;
    }

    cmd_stack.pop_back();
    cmd_stack.pop_back();
    if (remain_backtrace){
      cout<<"remaining in backtrace"<<endl;
      cmd_stack.push_back(tie("U", get<1>(u_tuple)));
    }else{
      cout<<"exiting backtrace"<<endl;
      backtrack=false;
    }

    }
  }
}

/**
 * @brief During backtracing, handle triplets.
 */
tuple<String, bool, bool> interpret_triple(tuple<String, String, String> triple) {
  String Y, U, X;
  tie(Y, U, X) = triple; 
  if (tie(Y, U, X) == make_tuple("R", "U", "L")) { 
    return make_tuple("L", false, true); 
  } else if (tie(Y, U, X) == make_tuple("L", "U", "T")) {
    return make_tuple("L", false, false);
  }
  else if (tie(Y, U, X) == make_tuple("R", "U", "T")) { 
    return make_tuple("R", false, false);
  } else if (tie(Y, U, X) == make_tuple("L", "U", "R")) {
    return make_tuple("R", false, true);
  } 
  else if (tie(Y, U, X) == make_tuple("T", "U", "L")) {
    return make_tuple("forward", false, false);
  } else if (tie(Y, U, X) == make_tuple("P", "U", "P")) {
    return make_tuple("forward", false, true);
  }
  else if (tie(Y, U, X) == make_tuple("L", "U", "L")) {
    return make_tuple("", true, true);//returning empty String instead of None
  } else if (tie(Y, U, X) == make_tuple("T", "U", "T")) {
    return make_tuple("", true, true);//we dont accept 4 ways
  } else if (tie(Y, U, X) == make_tuple("T", "U", "R")) {
    return make_tuple("", true, true);//we turn left by policy, so we wouldnt see this
  } else {
    return make_tuple("", true, true);
  }
}

/**
 * @brief copy oldNode's info onto trueNode.
 * update the neighbors of oldNode.
 */
void merge_two_nodes(shared_ptr<Node> trueNode,shared_ptr<Node> oldNode){
  for(int i=0; i< trueNode->ptrs.size();i++){

      if(oldNode->ptrs[i]!=nullptr && oldNode->ptrs[i]!=wall){

        trueNode->ptrs[i]=oldNode->ptrs[i];
        trueNode->costs[i]=max(oldNode->costs[i],trueNode->costs[i]);

        int j=(i+2)%4;
        oldNode->ptrs[i]->ptrs[j]=trueNode;
        oldNode->ptrs[i]->costs[j]=trueNode->costs[i];
      } 
    
  }
  graph.erase(graph.begin() + findNodeIndex(oldNode->uid));
  currentNodeIndex=findNodeIndex(trueNode->uid);

}



vector<shared_ptr<Node>> djikstra(String startName, String endName){
  vector<shared_ptr<Node>> unvisited;
  unordered_map<shared_ptr<Node>, shared_ptr<Node>> parentMap; 
  unordered_map<shared_ptr<Node>, int> distances;

  if(findNodeIndex(startName)==-1){ cerr<<"cant find start node"<<endl;return unvisited;}
  if(findNodeIndex(endName)==-1){ cerr<<"cant find end node"<<endl;return unvisited;}

  auto get_smallest_distance_node = [&](const vector<shared_ptr<Node>> nodes){
    shared_ptr<Node> best_node;
    int min_dist =numeric_limits<int>::max();
    for(int i=0;i<nodes.size();i++){
      if(distances[nodes[i]]<min_dist){
        min_dist = distances[nodes[i]];
        best_node = nodes[i];
      }
    }
    return best_node;
  }; 

  for (int i=0; i<graph.size();i++){
    distances[graph[i]]=numeric_limits<int>::max();
    unvisited.push_back(graph[i]);
  }
  distances[graph[findNodeIndex(startName)]]=0;

  while(!unvisited.empty()){
    shared_ptr<Node> current_node = get_smallest_distance_node(unvisited);

    if(distances[current_node] == numeric_limits<int>::max()) break;

    unvisited.erase(std::remove(unvisited.begin(), unvisited.end(), current_node), unvisited.end());

    for (int i=0;i<current_node->ptrs.size();i++){
      shared_ptr<Node> neighbor=current_node->ptrs[i];
      int cost= current_node->costs[i];

      if(neighbor==nullptr || neighbor==wall) continue;

      int alt = distances[current_node] + cost;
      if(alt < distances[neighbor]){
        distances[neighbor]=alt;
        parentMap[neighbor]=current_node;
      }
    }
    if(current_node->uid==endName) break;
  }

vector<shared_ptr<Node>> path;
if (distances[graph[findNodeIndex(endName)]] == std::numeric_limits<int>::max()) {
  cerr<<"end node distance still has intmax value"<<endl;
  return path;
  }
shared_ptr<Node> target_node=graph[findNodeIndex(endName)];
for (shared_ptr<Node> node = target_node;node!=nullptr;node=parentMap[node]){
        path.push_back(node);
      }


reverse(path.begin(), path.end());
cout<<"successful djikstra run"<<endl;
return path;
}

void printFinalGraphState() {
    unordered_set<shared_ptr<Node>> visited;
    queue<shared_ptr<Node>> q;

    if (graph.empty()) {
        cout << "(Empty graph)\n";
        return;
    }

    q.push(graph[0]); // Start from the first node in the graph

    while (!q.empty()) {
        shared_ptr<Node> node = q.front();
        q.pop();

        if (visited.find(node) != visited.end()) continue;
        visited.insert(node);

        if(node==nullptr || node==wall){
          continue;
        }
      
        cout << node->uid << ": { ";
        
        for (int i = 0; i < 4; i++) {
            if (node->ptrs[i]!=nullptr && node->ptrs[i]!=wall) {
                cout << (i == 0 ? "N: " : i == 1 ? "E: " : i == 2 ? "S: " : "W: ");
                cout << "(" << node->ptrs[i]->uid << ", Cost: " << node->costs[i] << ")";
            } else {
                cout << (i == 0 ? "N: None" : i == 1 ? "E: None" : i == 2 ? "S: None" : "W: None");
            }
            if (i < 3) cout << ", ";
        }

        cout << " }\n";

        // Enqueue neighbors for BFS
        for (int i = 0; i < 4; i++) {
            if (node->ptrs[i]!=nullptr && node->ptrs[i]!=wall && visited.find(node->ptrs[i]) == visited.end()) {
                q.push(node->ptrs[i]);
            }
        }
    }
}

String getFinalGraphString(){
  std::unordered_set<std::shared_ptr<Node>> visited;
    std::queue<std::shared_ptr<Node>> q;
    String result;

    if (graph.empty()) {
        result +="(Empty graph)\n";
        return result;
    }

    q.push(graph[0]); // Start from the first node in the graph

    while (!q.empty()) {
        std::shared_ptr<Node> node = q.front();
        q.pop();

        if (visited.find(node) != visited.end()) continue;
        visited.insert(node);

        if (node == nullptr || node == wall) {
            continue;
        }

        result += node->uid;
        result += ": { ";

        for (int i = 0; i < 4; i++) {
            if (node->ptrs[i] != nullptr && node->ptrs[i] != wall) {
                result += (i == 0 ? "N: " : i == 1 ? "E: " : i == 2 ? "S: " : "W: ");
                result += "(";
                result += node->ptrs[i]->uid;
                result += ", Cost: ";
                result += node->costs[i];
                result += ")";
            } else {
                result += (i == 0 ? "N: None" : i == 1 ? "E: None" : i == 2 ? "S: None" : "W: None");
            }
            if (i < 3) result += ", ";
        }

        result += " }\n";

        // Enqueue neighbors for BFS
        for (int i = 0; i < 4; i++) {
            if (node->ptrs[i] != nullptr && node->ptrs[i] != wall && visited.find(node->ptrs[i]) == visited.end()) {
                q.push(node->ptrs[i]);
            }
        }
    }

    return result;
}


void updateGraphInDatabase(String graph){
  // Path to the Map object in the database
  String mapPath = "/Map";

  // Read the existing Map object from the database
  FirebaseJson mapJson;
  if (Firebase.RTDB.getJSON(&fbdo, mapPath)) {
    mapJson = fbdo.jsonObject();
    Serial.println("Successfully fetched the Map object.");
  } else {
    Serial.printf("Failed to fetch Map: %s\n", fbdo.errorReason().c_str());
    return; // Exit if the fetch fails
  }

  // Generate a unique key for the new map (e.g., "Map2")
  int mapCount = mapJson.iteratorBegin();
  
MB_String newMapKey;
newMapKey="Map";
newMapKey += mapCount + 1;

  mapJson.iteratorEnd(); // Close iterator

  // Add the new graph string to the Map object
  mapJson.set(newMapKey.c_str(), graph);

  // Write the updated Map object back to the database
  if (Firebase.RTDB.set(&fbdo, mapPath, &mapJson)) {
    Serial.println("Successfully updated the Map object in the database.");
  } else {
    Serial.printf("Failed to update Map: %s\n", fbdo.errorReason().c_str());
  }
}

void updateTaskNodes() {
  // Ensure targetpath has at least one node
  if (targetpath.empty()) {
    Serial.println("Target path is empty. Nothing to update.");
    return;
  }

  String taskNodesStream;

  // Build the task nodes string
  for (size_t i = 0; i < targetpath.size(); ++i) {
    if (i > 0) {
      // Calculate the cost from the previous node to the current node
      auto previousNode = targetpath[i - 1];
      auto currentNode = targetpath[i];
      int cost;
      for(int i=0;i<previousNode->ptrs.size();i++){
        if(previousNode->ptrs[i]==currentNode){
          cost = previousNode->costs[i];
        }
      }
      
      

      // Append to the task nodes string
      taskNodesStream += "{";
      taskNodesStream += currentNode->uid;
      taskNodesStream += ",cost:";
      taskNodesStream += cost;
      taskNodesStream +="}";
    } else {
      // For the first node, only add its UID (no cost)
      taskNodesStream += "{";
      taskNodesStream +=targetpath[0]->uid;
      taskNodesStream += ",cost:0}";
    }
  }



  if (Firebase.RTDB.setString(&fbdo, "/Robot/task_nodes", taskNodesStream)) {
    Serial.println("Updated task nodes successfully.");
  } else {
    Serial.printf("Failed to update task nodes: %s\n", fbdo.errorReason().c_str());
  }

}