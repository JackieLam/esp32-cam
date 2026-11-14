// Demo 1: 板载 LED 闪烁 + 串口打印，用来验证板子、数据线、IDE 设置都 OK
const int LED = 3;  // DFR1154 板载 LED = GPIO3

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
}

void loop() {
  digitalWrite(LED, HIGH);
  Serial.println("LED ON");
  delay(500);
  digitalWrite(LED, LOW);
  Serial.println("LED OFF");
  delay(500);
}
