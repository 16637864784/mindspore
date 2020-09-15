/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MSNetWork.h"
#include <android/log.h>
#include <iostream>
#include <string>
#include "include/errorcode.h"

#define MS_PRINT(format, ...) __android_log_print(ANDROID_LOG_INFO, "MSJNI", format, ##__VA_ARGS__)

MSNetWork::MSNetWork(void) : session(nullptr), model(nullptr) {}

MSNetWork::~MSNetWork(void) {}

void
MSNetWork::CreateSessionMS(char *modelBuffer, size_t bufferLen, mindspore::lite::Context *ctx) {
  session = mindspore::session::LiteSession::CreateSession(ctx);
  if (session == nullptr) {
    MS_PRINT("Create Session failed.");
    return;
  }

  // Compile model.
  auto model = mindspore::lite::Model::Import(modelBuffer, bufferLen);
  if (model == nullptr) {
    MS_PRINT("Import model failed.");
    return;
  }

  int ret = session->CompileGraph(model);
  if (ret != mindspore::lite::RET_OK) {
    MS_PRINT("CompileGraph failed.");
    return;
  }
}

int MSNetWork::ReleaseNets(void) {
  delete session;
//    delete model;
  return 0;
}

const char *MSNetWork::labels_name_map[MSNetWork::RET_CATEGORY_SUM] = {
    {"Tortoise"}, {"Container"}, {"Magpie"}, {"Seaturtle"}, {"Football"}, {"Ambulance"}, {"Ladder"},
    {"Toothbrush"}, {"Syringe"}, {"Sink"}, {"Toy"}, {"Organ(MusicalInstrument) "}, {"Cassettedeck"},
    {"Apple"}, {"Humaneye"}, {"Cosmetics"}, {"Paddle"}, {"Snowman"}, {"Beer"}, {"Chopsticks"},
    {"Humanbeard"}, {"Bird"}, {"Parkingmeter"}, {"Trafficlight"}, {"Croissant"}, {"Cucumber"},
    {"Radish"}, {"Towel"}, {"Doll"}, {"Skull"}, {"Washingmachine"}, {"Glove"}, {"Tick"}, {"Belt"},
    {"Sunglasses"}, {"Banjo"}, {"Cart"}, {"Ball"}, {"Backpack"}, {"Bicycle"}, {"Homeappliance"},
    {"Centipede"}, {"Boat"}, {"Surfboard"}, {"Boot"}, {"Headphones"}, {"Hotdog"}, {"Shorts"},
    {"Fastfood"}, {"Bus"}, {"Boy "}, {"Screwdriver"}, {"Bicyclewheel"}, {"Barge"}, {"Laptop"},
    {"Miniskirt"}, {"Drill(Tool)"}, {"Dress"}, {"Bear"}, {"Waffle"}, {"Pancake"}, {"Brownbear"},
    {"Woodpecker"}, {"Bluejay"}, {"Pretzel"}, {"Bagel"}, {"Tower"}, {"Teapot"}, {"Person"},
    {"Bowandarrow"}, {"Swimwear"}, {"Beehive"}, {"Brassiere"}, {"Bee"}, {"Bat(Animal)"},
    {"Starfish"}, {"Popcorn"}, {"Burrito"}, {"Chainsaw"}, {"Balloon"}, {"Wrench"}, {"Tent"},
    {"Vehicleregistrationplate"}, {"Lantern"}, {"Toaster"}, {"Flashlight"}, {"Billboard"},
    {"Tiara"}, {"Limousine"}, {"Necklace"}, {"Carnivore"}, {"Scissors"}, {"Stairs"},
    {"Computerkeyboard"}, {"Printer"}, {"Trafficsign"}, {"Chair"}, {"Shirt"}, {"Poster"},
    {"Cheese"}, {"Sock"}, {"Firehydrant"}, {"Landvehicle"}, {"Earrings"}, {"Tie"}, {"Watercraft"},
    {"Cabinetry"}, {"Suitcase"}, {"Muffin"}, {"Bidet"}, {"Snack"}, {"Snowmobile"}, {"Clock"},
    {"Medicalequipment"}, {"Cattle"}, {"Cello"}, {"Jetski"}, {"Camel"}, {"Coat"}, {"Suit"},
    {"Desk"}, {"Cat"}, {"Bronzesculpture"}, {"Juice"}, {"Gondola"}, {"Beetle"}, {"Cannon"},
    {"Computermouse"}, {"Cookie"}, {"Officebuilding"}, {"Fountain"}, {"Coin"}, {"Calculator"},
    {"Cocktail"}, {"Computermonitor"}, {"Box"}, {"Stapler"}, {"Christmastree"}, {"Cowboyhat"},
    {"Hikingequipment"}, {"Studiocouch"}, {"Drum"}, {"Dessert"}, {"Winerack"}, {"Drink"},
    {"Zucchini"}, {"Ladle"}, {"Humanmouth"}, {"DairyProduct"}, {"Dice"}, {"Oven"}, {"Dinosaur"},
    {"Ratchet(Device)"}, {"Couch"}, {"Cricketball"}, {"Wintermelon"}, {"Spatula"}, {"Whiteboard"},
    {"Pencilsharpener"}, {"Door"}, {"Hat"}, {"Shower"}, {"Eraser"}, {"Fedora"}, {"Guacamole"},
    {"Dagger"}, {"Scarf"}, {"Dolphin"}, {"Sombrero"}, {"Tincan"}, {"Mug"}, {"Tap"}, {"Harborseal"},
    {"Stretcher"}, {"Canopener"}, {"Goggles"}, {"Humanbody"}, {"Rollerskates"}, {"Coffeecup"},
    {"Cuttingboard"}, {"Blender"}, {"Plumbingfixture"}, {"Stopsign"}, {"Officesupplies"},
    {"Volleyball(Ball)"}, {"Vase"}, {"Slowcooker"}, {"Wardrobe"}, {"Coffee"}, {"Whisk"},
    {"Papertowel"}, {"Personalcare"}, {"Food"}, {"Sunhat"}, {"Treehouse"}, {"Flyingdisc"},
    {"Skirt"}, {"Gasstove"}, {"Saltandpeppershakers"}, {"Mechanicalfan"}, {"Facepowder"}, {"Fax"},
    {"Fruit"}, {"Frenchfries"}, {"Nightstand"}, {"Barrel"}, {"Kite"}, {"Tart"}, {"Treadmill"},
    {"Fox"}, {"Flag"}, {"Frenchhorn"}, {"Windowblind"}, {"Humanfoot"}, {"Golfcart"}, {"Jacket"},
    {"Egg(Food)"}, {"Streetlight"}, {"Guitar"}, {"Pillow"}, {"Humanleg"}, {"Isopod"}, {"Grape"},
    {"Humanear"}, {"Powerplugsandsockets"}, {"Panda"}, {"Giraffe"}, {"Woman"}, {"Doorhandle"},
    {"Rhinoceros"}, {"Bathtub"}, {"Goldfish"}, {"Houseplant"}, {"Goat"}, {"Baseballbat"},
    {"Baseballglove"}, {"Mixingbowl"}, {"Marineinvertebrates"}, {"Kitchenutensil"}, {"Lightswitch"},
    {"House"}, {"Horse"}, {"Stationarybicycle"}, {"Hammer"}, {"Ceilingfan"}, {"Sofabed"},
    {"Adhesivetape "}, {"Harp"}, {"Sandal"}, {"Bicyclehelmet"}, {"Saucer"}, {"Harpsichord"},
    {"Humanhair"}, {"Heater"}, {"Harmonica"}, {"Hamster"}, {"Curtain"}, {"Bed"}, {"Kettle"},
    {"Fireplace"}, {"Scale"}, {"Drinkingstraw"}, {"Insect"}, {"Hairdryer"}, {"Kitchenware"},
    {"Indoorrower"}, {"Invertebrate"}, {"Foodprocessor"}, {"Bookcase"}, {"Refrigerator"},
    {"Wood-burningstove"}, {"Punchingbag"}, {"Commonfig"}, {"Cocktailshaker"}, {"Jaguar(Animal)"},
    {"Golfball"}, {"Fashionaccessory"}, {"Alarmclock"}, {"Filingcabinet"}, {"Artichoke"}, {"Table"},
    {"Tableware"}, {"Kangaroo"}, {"Koala"}, {"Knife"}, {"Bottle"}, {"Bottleopener"}, {"Lynx"},
    {"Lavender(Plant)"}, {"Lighthouse"}, {"Dumbbell"}, {"Humanhead"}, {"Bowl"}, {"Humidifier"},
    {"Porch"}, {"Lizard"}, {"Billiardtable"}, {"Mammal"}, {"Mouse"}, {"Motorcycle"},
    {"Musicalinstrument"}, {"Swimcap"}, {"Fryingpan"}, {"Snowplow"}, {"Bathroomcabinet"},
    {"Missile"}, {"Bust"}, {"Man"}, {"Waffleiron"}, {"Milk"}, {"Ringbinder"}, {"Plate"},
    {"Mobilephone"}, {"Bakedgoods"}, {"Mushroom"}, {"Crutch"}, {"Pitcher(Container)"}, {"Mirror"},
    {"Personalflotationdevice"}, {"Tabletennisracket"}, {"Pencilcase"}, {"Musicalkeyboard"},
    {"Scoreboard"}, {"Briefcase"}, {"Kitchenknife"}, {"Nail(Construction)"}, {"Tennisball"},
    {"Plasticbag"}, {"Oboe"}, {"Chestofdrawers"}, {"Ostrich"}, {"Piano"}, {"Girl"}, {"Plant"},
    {"Potato"}, {"Hairspray"}, {"Sportsequipment"}, {"Pasta"}, {"Penguin"}, {"Pumpkin"}, {"Pear"},
    {"Infantbed"}, {"Polarbear"}, {"Mixer"}, {"Cupboard"}, {"Jacuzzi"}, {"Pizza"}, {"Digitalclock"},
    {"Pig"}, {"Reptile"}, {"Rifle"}, {"Lipstick"}, {"Skateboard"}, {"Raven"}, {"Highheels"},
    {"Redpanda"}, {"Rose"}, {"Rabbit"}, {"Sculpture"}, {"Saxophone"}, {"Shotgun"}, {"Seafood"},
    {"Submarinesandwich"}, {"Snowboard"}, {"Sword"}, {"Pictureframe"}, {"Sushi"}, {"Loveseat"},
    {"Ski"}, {"Squirrel"}, {"Tripod"}, {"Stethoscope"}, {"Submarine"}, {"Scorpion"}, {"Segway"},
    {"Trainingbench"}, {"Snake"}, {"Coffeetable"}, {"Skyscraper"}, {"Sheep"}, {"Television"},
    {"Trombone"}, {"Tea"}, {"Tank"}, {"Taco"}, {"Telephone"}, {"Torch"}, {"Tiger"}, {"Strawberry"},
    {"Trumpet"}, {"Tree"}, {"Tomato"}, {"Train"}, {"Tool"}, {"Picnicbasket"}, {"Cookingspray"},
    {"Trousers"}, {"Bowlingequipment"}, {"Footballhelmet"}, {"Truck"}, {"Measuringcup"},
    {"Coffeemaker"}, {"Violin"}, {"Vehicle"}, {"Handbag"}, {"Papercutter"}, {"Wine"}, {"Weapon"},
    {"Wheel"}, {"Worm"}, {"Wok"}, {"Whale"}, {"Zebra"}, {"Autopart"}, {"Jug"}, {"Pizzacutter"},
    {"Cream"}, {"Monkey"}, {"Lion"}, {"Bread"}, {"Platter"}, {"Chicken"}, {"Eagle"}, {"Helicopter"},
    {"Owl"}, {"Duck"}, {"Turtle"}, {"Hippopotamus"}, {"Crocodile"}, {"Toilet"}, {"Toiletpaper"},
    {"Squid"}, {"Clothing"}, {"Footwear"}, {"Lemon"}, {"Spider"}, {"Deer"}, {"Frog"}, {"Banana"},
    {"Rocket"}, {"Wineglass"}, {"Countertop"}, {"Tabletcomputer"}, {"Wastecontainer"},
    {"Swimmingpool"}, {"Dog"}, {"Book"}, {"Elephant"}, {"Shark"}, {"Candle"}, {"Leopard"}, {"Axe"},
    {"Handdryer"}, {"Soapdispenser"}, {"Porcupine"}, {"Flower"}, {"Canary"}, {"Cheetah"},
    {"Palmtree"}, {"Hamburger"}, {"Maple"}, {"Building"}, {"Fish"}, {"Lobster"},
    {"GardenAsparagus"}, {"Furniture"}, {"Hedgehog"}, {"Airplane"}, {"Spoon"}, {"Otter"}, {"Bull"},
    {"Oyster"}, {"Horizontalbar"}, {"Conveniencestore"}, {"Bomb"}, {"Bench"}, {"Icecream"},
    {"Caterpillar"}, {"Butterfly"}, {"Parachute"}, {"Orange"}, {"Antelope"}, {"Beaker"},
    {"Mothsandbutterflies"}, {"Window"}, {"Closet"}, {"Castle"}, {"Jellyfish"}, {"Goose"}, {"Mule"},
    {"Swan"}, {"Peach"}, {"Coconut"}, {"Seatbelt"}, {"Raccoon"}, {"Chisel"}, {"Fork"}, {"Lamp"},
    {"Camera"}, {"Squash(Plant)"}, {"Racket"}, {"Humanface"}, {"Humanarm"}, {"Vegetable"},
    {"Diaper"}, {"Unicycle"}, {"Falcon"}, {"Chime"}, {"Snail"}, {"Shellfish"}, {"Cabbage"},
    {"Carrot"}, {"Mango"}, {"Jeans"}, {"Flowerpot"}, {"Pineapple"}, {"Drawer"}, {"Stool"},
    {"Envelope"}, {"Cake"}, {"Dragonfly"}, {"Commonsunflower"}, {"Microwaveoven"}, {"Honeycomb"},
    {"Marinemammal"}, {"Sealion"}, {"Ladybug"}, {"Shelf"}, {"Watch"}, {"Candy"}, {"Salad"},
    {"Parrot"}, {"Handgun"}, {"Sparrow"}, {"Van"}, {"Grinder"}, {"Spicerack"}, {"Lightbulb"},
    {"Cordedphone"}, {"Sportsuniform"}, {"Tennisracket"}, {"Wallclock"}, {"Servingtray"},
    {"Kitchen&diningroomtable"}, {"Dogbed"}, {"Cakestand"}, {"Catfurniture"}, {"Bathroomaccessory"},
    {"Facialtissueholder"}, {"Pressurecooker"}, {"Kitchenappliance"}, {"Tire"}, {"Ruler"},
    {"Luggageandbags"}, {"Microphone"}, {"Broccoli"}, {"Umbrella"}, {"Pastry"}, {"Grapefruit"},
    {"Band-aid"}, {"Animal"}, {"Bellpepper"}, {"Turkey"}, {"Lily"}, {"Pomegranate"}, {"Doughnut"},
    {"Glasses"}, {"Humannose"}, {"Pen"}, {"Ant"}, {"Car"}, {"Aircraft"}, {"Humanhand"}, {"Skunk"},
    {"Teddybear"}, {"Watermelon"}, {"Cantaloupe"}, {"Dishwasher"}, {"Flute"}, {"Balancebeam"},
    {"Sandwich"}, {"Shrimp"}, {"Sewingmachine"}, {"Binoculars"}, {"Raysandskates"}, {"Ipod"},
    {"Accordion"}, {"Willow"}, {"Crab"}, {"Crown"}, {"Seahorse"}, {"Perfume"}, {"Alpaca"}, {"Taxi"},
    {"Canoe"}, {"Remotecontrol"}, {"Wheelchair"}, {"Rugbyball"}, {"Armadillo"}, {"Maracas"},
    {"Helmet"}};

