(ns jepsen.memgraph.bank
  "Bank account test on Memgraph.
  The test should do random transfers on
  the main instance while randomly reading
  the total sum of the all nodes which
  should be consistent."
  (:require [neo4j-clj.core :as dbclient]
            [clojure.tools.logging :refer [info]]
            [clojure.string :as string]
            [clojure.core.reducers :as r]
            [jepsen
             [store :as store]
             [checker :as checker]
             [generator :as gen]
             [client :as client]
             [history :as h]
             [util :as util]]
            [jepsen.checker.timeline :as timeline]
            [jepsen.checker.perf :as perf]
            [jepsen.memgraph.client :as mgclient]
            [jepsen.memgraph.utils :as utils]))

(defn transfer-money
  "Transfer money from one account to another by some amount
  if the account you're transfering money from has enough
  money."
  [conn from to amount]
  (dbclient/with-transaction conn tx
    (when (-> (mgclient/get-account tx {:id from}) first :n :balance (>= amount))
      (mgclient/update-balance tx {:id from :amount (- amount)})
      (mgclient/update-balance tx {:id to :amount amount}))))

(defrecord Client [nodes-config]
  client/Client
  ; Open connection to the node. Setup each node.
  (open! [this _test node]
    (info "Opening connection to node" node)
    (mgclient/replication-open-connection this node nodes-config))
  ; On main detach-delete-all and create accounts.
  (setup! [this _test]
    (when (= (:replication-role this) :main)
      (try
        (utils/with-session (:conn this) session
          (do
            (mgclient/detach-delete-all session)
            (info "Creating" utils/account-num "accounts")
            (dotimes [i utils/account-num]
              (info "Creating account:" i)
              (mgclient/create-account session {:id i :balance utils/starting-balance}))))
        (catch org.neo4j.driver.exceptions.ServiceUnavailableException _e
          (info (utils/node-is-down (:node this)))))))
  (invoke! [this _test op]
    (case (:f op)
      ; Create a map with the following structure: {:type :ok :value {:accounts [account1 account2 ...] :node node}}
      ; Read always succeeds and returns all accounts.
      ; Node is a variable, not an argument to the function. It indicated current node on which action :read is being executed.
      :read-balances
      (try
        (utils/with-session (:conn this) session
          (let [accounts (->> (mgclient/get-all-accounts session) (map :n) (reduce conj []))
                total (reduce + (map :balance accounts))]
            (assoc op
                   :type :ok
                   :value {:accounts accounts
                           :node (:node this)
                           :total total
                           :correct (= total (* utils/account-num utils/starting-balance))})))
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
                  (assoc op :type :info :value "Not main node."))

      ; Transfer money from one account to another. Only executed on main.
      ; If the transferring succeeds, return :ok, otherwise return :fail.
      ; Transfer will fail if the account doesn't exist or if the account doesn't have enough or if update-balance
      ; doesn't return anything.
      ; Allow the exception due to down sync replica.
      :transfer (if (= (:replication-role this) :main)
                  (try
                    (let [transfer-info (:value op)]
                      (transfer-money
                       (:conn this)
                       (:from transfer-info)
                       (:to transfer-info)
                       (:amount transfer-info)))
                    (assoc op :type :ok)
                    (catch org.neo4j.driver.exceptions.ServiceUnavailableException _e
                      (utils/process-service-unavilable-exc op (:node this)))
                    (catch Exception e
                      (if (string/includes? (str e) "At least one SYNC replica has not confirmed committing last transaction.")
                        (assoc op :type :ok :value (str e)); Exception due to down sync replica is accepted/expected
                        (assoc op :type :fail :value (str e)))))
                  (assoc op :type :info :value "Not main node."))))
  ; On teardown! only main will detach-delete-all.
  (teardown! [this _test]
    (when (= (:replication-role this) :main)
      (utils/with-session (:conn this) session
        (try
          (mgclient/detach-delete-all session)
          (catch Exception _)))))
  (close! [this _test]
    (dbclient/disconnect (:conn this))))

(defn bank-checker
  "Balances must all be non-negative and sum to the model's total
  Each node should have at least one read that returned all accounts.
  We allow the reads to be empty because the replica can connect to
  main at some later point, until that point the replica is empty."
  []
  (reify checker/Checker
    (check [_ _ history _]
      (let [ok-reads  (->> history
                           (filter #(= :ok (:type %)))
                           (filter #(= :read-balances (:f %))))

            bad-reads (utils/analyze-bank-data-reads ok-reads utils/account-num utils/starting-balance)

            empty-nodes (utils/analyze-empty-data-nodes ok-reads)

            failed-read-balances (->> history
                                      (filter #(= :fail (:type %)))
                                      (filter #(= :read-balances (:f %)))
                                      (map :value))

            failed-transfers (->> history
                                  (filter #(= :fail (:type %)))
                                  (filter #(= :transfer (:f %)))
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
                            :empty-failed-read-balances? (empty? failed-read-balances)
                            :empty-failed-transfers? (empty? failed-transfers)
                            :empty-failed-registrations? (empty? failed-registrations)}

            updates [{:key :empty-nodes :condition (not (:empty-nodes? initial-result)) :value empty-nodes}
                     {:key :empty-bad-reads :condition (not (:empty-bad-reads? initial-result)) :value bad-reads}
                     {:key :empty-failed-read-balances :condition (not (:empty-failed-read-balances? initial-result)) :value failed-read-balances}
                     {:key :empty-failed-transfers :condition (not (:empty-failed-transfers? initial-result)) :value failed-transfers}
                     {:key :empty-failed-registrations :condition (not (:empty-failed-registrations? initial-result)) :value failed-registrations}]]

        (reduce (fn [result update]
                  (if (:condition update)
                    (assoc result (:key update) (:value update))
                    result))
                initial-result
                updates)))))

(defn ok-reads
  "Filters a history to just OK reads. Returns nil if there are none."
  [history]
  (let [h (filter #(and (h/ok? %)
                        (= :read-balances (:f %)))
                  history)]
    (when (seq h)
      (vec h))))

(defn by-node
  "Groups operations by node."
  [test history]
  (let [nodes (:nodes test)
        n     (count nodes)]
    (->> history
         (r/filter (comp number? :process))
         (group-by (fn [op]
                     (let [p (:process op)]
                       (nth nodes (mod p n))))))))

(defn points
  "Turns a history into a seqeunce of [time total-of-accounts] points."
  [history]
  (mapv (fn [op]
          [(util/nanos->secs (:time op))
           (:total (:value op))])
        history))

(defn plotter
  "Renders a graph of balances over time"
  []
  (reify checker/Checker
    (check [_ test history opts]
      (when-let [reads (ok-reads history)]
        (let [totals (->> reads
                          (by-node test)
                          (util/map-vals points))
              colors (perf/qs->colors (keys totals))
              path (.getCanonicalPath
                    (store/path! test (:subdirectory opts) "bank.png"))
              preamble (concat (perf/preamble path)
                               [['set 'title (str (:name test) " bank")]
                                '[set ylabel "Total of all accounts"]])
              series (for [[node data] totals]
                       {:title      node
                        :with       :points
                        :pointtype  2
                        :linetype   (colors node)
                        :data       data})]
          (-> {:preamble  preamble
               :series    series}
              (perf/with-range)
              (perf/with-nemeses history (:nemeses (:plot test)))
              perf/plot!)
          {:valid? true})))))

(defn workload
  "Basic test workload"
  [opts]
  {:client    (Client. (:nodes-config opts))
   :checker   (checker/compose
               {:bank     (bank-checker)
                :timeline (timeline/html)
                :plot     (plotter)})
   :generator (mgclient/replication-gen (gen/mix [utils/read-balances utils/valid-transfer]))
   :final-generator {:clients (gen/once utils/read-balances) :recovery-time 20}})
