(ns jepsen.memgraph.large
  "Large write test"
  (:require [neo4j-clj.core :as dbclient]
            [clojure.tools.logging :refer [info]]
            [clojure.string :as string]
            [jepsen
             [checker :as checker]
             [client :as client]
             [generator :as gen]]
            [jepsen.checker.timeline :as timeline]
            [jepsen.memgraph.utils :as utils]
            [jepsen.memgraph.client :as mgclient]))

; It is important that at least once applying deltas passes to replicas. Before this value was 100k so the instance never had
; enough time to apply all deltas.
(def node-num 5000)

(dbclient/defquery get-node-count
  "MATCH (n:Node) RETURN count(n) as c;")

(defn create-nodes-builder
  []
  (dbclient/create-query
   (str "UNWIND range(1, " node-num ") AS i CREATE (n:Node {id: i, property1: 0, property2: 1, property3: 2});")))

(def create-nodes (create-nodes-builder))

(defrecord Client [nodes-config]
  client/Client
  (open! [this _test node]
    (mgclient/replication-open-connection this node nodes-config))
  (setup! [this _test]
    (when (= (:replication-role this) :main)
      (try
        (utils/with-session (:conn this) session
          (mgclient/detach-delete-all session)
          (info "Initial nodes deleted.")
          (create-nodes session)
          (info "Initial nodes created."))
        (catch org.neo4j.driver.exceptions.ServiceUnavailableException _e
          (info (utils/node-is-down (:node this)))))))

  (invoke! [this _test op]
    (case (:f op)
      :read
      (try
        (utils/with-session (:conn this) session
          (assoc op
                 :type :ok
                 :value {:count (->> (get-node-count session)
                                     first
                                     :c)
                         :node (:node this)}))
        (catch org.neo4j.driver.exceptions.ServiceUnavailableException _e
          (utils/process-service-unavilable-exc op (:node this)))
        (catch Exception e
          (assoc op :type :fail :value (str e))))

      :register (if (= (:replication-role this) :main)
                  (do
                    (doseq [n (filter #(= (:replication-role (val %))
                                          :replica)
                                      nodes-config)]
                      (try
                        (utils/with-session (:conn this) session
                          ((mgclient/create-register-replica-query
                            (first n)
                            (second n)) session))
                        (catch Exception _e)))
                    (assoc op :type :ok))
                  (assoc op :type :info :value "Not main node"))

      ; When executed on main, create nodes.
      :add    (if (= (:replication-role this) :main)
                (utils/with-session (:conn this) session
                  (try
                    ((create-nodes session)
                     (assoc op :type :ok :value "Nodes created."))
                    (catch org.neo4j.driver.exceptions.ServiceUnavailableException _e
                      (assoc op :type :info :value (utils/node-is-down (:node this))))
                    (catch Exception e
                      (if (string/includes? (str e) "At least one SYNC replica has not confirmed committing last transaction.")
                        (assoc op :type :ok :value (str e)); Exception due to down sync replica is accepted/expected
                        (assoc op :type :fail :value (str e))))))
                (assoc op :type :info :info "Not main node"))))
  (teardown! [this _test]
    (when (= (:replication-role this) :main)
      (utils/with-session (:conn this) session
        (try
          ; Can fail for various reasons, not important at this point.
          (mgclient/detach-delete-all session)
          (catch Exception _)))))
  (close! [this _test]
    (dbclient/disconnect (:conn this))))

(defn add-nodes
  "Add nodes"
  [_ _]
  {:type :invoke :f :add :value nil})

(defn read-nodes
  "Read node count"
  [_ _]
  {:type :invoke :f :read :value nil})

(defn large-checker
  "Check if every read has a count divisible with node-num."
  []
  (reify checker/Checker
    (check [_ _ history _]
      ; For OK reads get all with :type :ok and :f :read.
      (let [ok-reads (->> history
                          (filter #(= :ok (:type %)))
                          (filter #(= :read (:f %))))
            ; Read is considered bad if count is not divisible with node-num.
            bad-reads (->> ok-reads
                           (map (fn [op]
                                  (let [count (-> op :value :count)]
                                    (when (not= 0 (mod count node-num))
                                      {:type :invalid-count
                                       :op op}))))
                           ; Filter nil values.
                           (filter identity)
                           (into []))
            ; First get all-nodes by mapping :value :node from ok-reads and storing it into a set.
            empty-nodes (let [all-nodes (->> ok-reads
                                             (map #(-> % :value :node))
                                             (reduce conj #{}))]
                          ; This code filters all-nodes to include only those nodes for which all associated ok-reads have a :count of 0.
                          (->> all-nodes
                               (filter (fn [node]
                                         (every?
                                          #(= 0 %)
                                          ; Filter all ok-reads by node, get its count and check it if it is 0.
                                          (->> ok-reads
                                               (map :value)
                                               (filter #(= node (:node %)))
                                               (map :count)))))
                               ; Filter nil values and save it into a vector.
                               (filter identity)
                               (into [])))

            failed-reads (->> history
                              (filter #(= :fail (:type %)))
                              (filter #(= :read (:f %)))
                              (map :value))

            failed-adds (->> history
                             (filter #(= :fail (:type %)))
                             (filter #(= :add (:f %)))
                             (map :value))

            failed-registrations (->> history
                                      (filter #(= :fail (:type %)))
                                      (filter #(= :register (:f %)))
                                      (map :value))

            initial-result {:valid? (and
                                     (empty? bad-reads)
                                     (empty? empty-nodes))
                            :empty-nodes? (empty? empty-nodes)
                            :empty-bad-reads? (empty? bad-reads)
                            :empty-failed-reads? (empty? failed-reads)
                            :empty-failed-adds? (empty? failed-adds)
                            :empty-failed-registrations? (empty? failed-registrations)}

            updates [{:key :empty-nodes :condition (not (:empty-nodes? initial-result)) :value empty-nodes}
                     {:key :empty-bad-reads :condition (not (:empty-bad-reads? initial-result)) :value bad-reads}
                     {:key :empty-failed-reads :condition (not (:empty-failed-reads? initial-result)) :value failed-reads}
                     {:key :empty-failed-adds :condition (not (:empty-failed-adds? initial-result)) :value failed-adds}
                     {:key :empty-failed-registrations :condition (not (:empty-failed-registrations? initial-result)) :value failed-registrations}]]

        (reduce (fn [result update]
                  (if (:condition update)
                    (assoc result (:key update) (:value update))
                    result))
                initial-result
                updates)))))

(defn workload
  [opts]
  {:client (Client. (:nodes-config opts))
   :checker (checker/compose
             {:large    (large-checker)
              :timeline (timeline/html)})
   :generator (mgclient/replication-gen
               (gen/mix [read-nodes add-nodes]))
   :final-generator {:clients (gen/once read-nodes) :recovery-time 40}})
